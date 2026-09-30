#include "game/player.hpp"
#include "game/memory.hpp"
#include "game/targets.hpp"
#include "game/weapon_ids.hpp"
#include "util/log.hpp"

#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace chaos::player {
namespace {

constexpr uintptr_t kFpCur = 0x148;
constexpr uintptr_t kFpMax = 0x150;
constexpr uintptr_t kFpMaxAlt = 0x14C;
constexpr uintptr_t kSoulsOff = 0x6C;

// PlayerGameData + 0x2B0 = EquipGameData (fromsoftware-rs / TGA).
constexpr uintptr_t kEquipGameData = 0x2B0;
constexpr uintptr_t kEquipIdxList = 0x08;  // u32[22] inventory indices
// ChrAsm sits at EquipGameData+0x70. Live ER has equipment_param_ids at ChrAsm+0x78
// (one int before the fromsoftware-rs 0x7C layout).
constexpr uintptr_t kChrAsm = 0x70;
constexpr uintptr_t kEquipParamIds = kChrAsm + 0x78;
constexpr uintptr_t kInvPtr = 0x5D0;     // TGA getInvData pointer
constexpr uintptr_t kInvInline = 0x408;  // EquipInventoryData by value
constexpr int kEquipSlotCount = 22;
constexpr int kUnarmedWep = 110000;
constexpr int kEmptyId = -1;
constexpr int kProtectorType = 0x10000000;

// TGA unequip dummies for armor (naked protectors, with type nibble).
constexpr int kNakedArmor[] = {
    kProtectorType + 10000,  // head
    kProtectorType + 10100,  // chest
    kProtectorType + 10200,  // hands
    kProtectorType + 10300,  // legs
};

std::string HexU(uintptr_t v) {
    char b[32];
    std::snprintf(b, sizeof(b), "%llX", static_cast<unsigned long long>(v));
    return b;
}

std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool PlausibleHp(int value) { return value >= 1 && value <= 50000; }
bool PlausibleStat(int value) { return value >= 1 && value <= 99; }
bool PlausibleFp(int value) { return value >= 0 && value <= 50000; }

std::optional<uintptr_t> ResolveGameDataMan(const Config& cfg) {
    const char* patterns[] = {
        cfg.gamedata_aob.c_str(),
        "48 8B 05 ?? ?? ?? ?? 48 85 C0 74 05 48 8B 40 58",
        "48 8B 05 ?? ?? ?? ?? 48 85 C0 74 05 48 8B 40 58 C3",
        "48 8B 05 ?? ?? ?? ?? 48 85 C0 74 08 48 8B 48 08",
        "48 8B 05 ?? ?? ?? ?? 48 85 C0 74 05 48 8B 40 08",
    };

    for (const char* pat : patterns) {
        if (!pat || !*pat) continue;
        auto hit = mem::FindAob(pat);
        if (!hit) continue;
        auto ptrSlot = mem::ResolveRip(*hit, 3, 7);
        if (!ptrSlot) continue;
        uintptr_t gdm = 0;
        if (!mem::ReadPtr(*ptrSlot, gdm) || !gdm) continue;
        uintptr_t pgd = 0;
        if (!mem::ReadPtr(gdm + cfg.gamedata_player_game_data, pgd) || !pgd) continue;
        int vigor = 0;
        if (!mem::Read(pgd + cfg.stats.vigor, vigor)) continue;
        if (vigor < 1 || vigor > 99) {
            if (!mem::Read(pgd + 0x3C, vigor) || vigor < 1 || vigor > 99) continue;
        }
        return gdm;
    }
    log::Error("GameDataMan AOB not found / failed validation");
    return std::nullopt;
}

std::optional<uintptr_t> LocalPlayerGameData(const Config& cfg) {
    auto gdm = ResolveGameDataMan(cfg);
    if (!gdm) return std::nullopt;
    uintptr_t pgd = 0;
    if (!mem::ReadPtr(*gdm + cfg.gamedata_player_game_data, pgd) || !pgd) return std::nullopt;
    return pgd;
}

std::optional<uintptr_t> StatOffset(const Config& cfg, const std::string& stat) {
    const auto s = Lower(stat);
    if (s == "vigor") return cfg.stats.vigor;
    if (s == "mind") return cfg.stats.mind;
    if (s == "endurance") return cfg.stats.endurance;
    if (s == "strength") return cfg.stats.strength;
    if (s == "dexterity") return cfg.stats.dexterity;
    if (s == "intelligence") return cfg.stats.intelligence;
    if (s == "faith") return cfg.stats.faith;
    if (s == "arcane") return cfg.stats.arcane;
    return std::nullopt;
}

bool HealOne(const targets::PlayerRef& p, const Config& cfg) {
    if (!p.char_data) return false;
    int cur = 0, mx = 0;
    if (!mem::Read(p.char_data + cfg.hp_current, cur)) return false;
    if (!mem::Read(p.char_data + cfg.hp_max, mx) || !PlausibleHp(mx)) {
        if (!mem::Read(p.char_data + 0x13C, mx) || !PlausibleHp(mx)) return false;
    }
    if (!mem::Write(p.char_data + cfg.hp_current, mx)) return false;
    log::Info("Heal slot=" + std::to_string(p.slot) + " " + std::to_string(cur) + "->" + std::to_string(mx));
    return true;
}

bool RestoreFpOne(const targets::PlayerRef& p) {
    if (!p.char_data) return false;
    int mx = 0;
    if (!mem::Read(p.char_data + kFpMax, mx) || !PlausibleFp(mx) || mx < 1) {
        if (!mem::Read(p.char_data + kFpMaxAlt, mx) || !PlausibleFp(mx) || mx < 1) return false;
    }
    int cur = 0;
    mem::Read(p.char_data + kFpCur, cur);
    if (!mem::Write(p.char_data + kFpCur, mx)) return false;
    log::Info("FP restore slot=" + std::to_string(p.slot) + " " + std::to_string(cur) + "->" + std::to_string(mx));
    return true;
}

bool ApplyStatOnPgd(uintptr_t pgd, uintptr_t off, int delta, const std::string& name) {
    const uintptr_t tries[] = {off, (off >= 4 ? off - 4 : off), off + 4};
    for (uintptr_t rel : tries) {
        int current = 0;
        if (!mem::Read(pgd + rel, current) || !PlausibleStat(current)) continue;
        int next = std::clamp(current + delta, 1, 99);
        if (!mem::Write(pgd + rel, next)) return false;
        log::Info(name + ": " + std::to_string(current) + " -> " + std::to_string(next));
        return true;
    }
    return false;
}

bool LoseRunesOnPgd(uintptr_t pgd, int percent) {
    int souls = 0;
    if (!mem::Read(pgd + kSoulsOff, souls) || souls < 0) return false;
    const int loss = static_cast<int>((static_cast<long long>(souls) * std::clamp(percent, 0, 100)) / 100);
    const int next = std::max(0, souls - loss);
    if (!mem::Write(pgd + kSoulsOff, next)) return false;
    log::Info("Runes: " + std::to_string(souls) + " -> " + std::to_string(next) + " (-" + std::to_string(loss) + ")");
    return true;
}

unsigned StripType(unsigned id) { return id & 0x0FFFFFFFu; }

bool IsEmptyMarker(int v) {
    return v == 0 || v == kEmptyId || v == static_cast<int>(0xFFFFFFFF);
}

bool ParamLooksEquipped(int slot, int param) {
    if (IsEmptyMarker(param)) return false;
    const unsigned p = StripType(static_cast<unsigned>(param));
    if (slot <= 5) return p != 0 && p != static_cast<unsigned>(kUnarmedWep);
    if (slot >= 12 && slot <= 15) {
        const unsigned naked[] = {10000u, 10100u, 10200u, 10300u};
        for (unsigned n : naked) {
            if (p == n) return false;
        }
        return p != 0;
    }
    if (slot >= 17 && slot <= 21) return p != 0;
    return p != 0;
}

bool SlotHasGear(int slot, int param) {
    // Unarmed/naked still have gaitem handles, so handles cannot mean "something is on".
    return ParamLooksEquipped(slot, param);
}

int ScoreParamLayout(uintptr_t eq, uintptr_t paramOff) {
    int score = 0;
    for (int slot = 0; slot < kEquipSlotCount; ++slot) {
        int param = 0;
        if (!mem::Read(eq + paramOff + static_cast<uintptr_t>(slot) * 4, param)) return -1;
        const unsigned p = StripType(static_cast<unsigned>(param));
        if (slot <= 5) {
            if (p == static_cast<unsigned>(kUnarmedWep) || (p >= 1000000u && p < 30000000u)) score += 2;
        } else if (slot >= 12 && slot <= 15) {
            if (p == 10000u || p == 10100u || p == 10200u || p == 10300u) score += 2;
            else if (p >= 20000u && p <= 20000000u) score += 4;
        }
    }
    return score;
}

uintptr_t ParamArrayOffset(uintptr_t pgd) {
    const uintptr_t eq = pgd + kEquipGameData;
    const uintptr_t offs[] = {kChrAsm + 0x78, kChrAsm + 0x7C, kChrAsm + 0x74, kChrAsm + 0x80};
    uintptr_t best = kEquipParamIds;
    int best_score = -1;
    for (uintptr_t off : offs) {
        const int s = ScoreParamLayout(eq, off);
        if (s > best_score) {
            best_score = s;
            best = off;
        }
    }
    log::Info("ChrAsm param ids at EquipGameData+0x" + HexU(best) +
              " score=" + std::to_string(best_score));
    return best;
}

bool ReadEquipSlot(uintptr_t pgd, uintptr_t paramOff, int slot, int& idx, int& param) {
    if (slot < 0 || slot >= kEquipSlotCount) return false;
    const uintptr_t eq = pgd + kEquipGameData;
    if (!mem::Read(eq + kEquipIdxList + static_cast<uintptr_t>(slot) * 4, idx)) idx = kEmptyId;
    if (!mem::Read(eq + paramOff + static_cast<uintptr_t>(slot) * 4, param)) param = 0;
    return true;
}

void LogEquipSnapshot(uintptr_t pgd, uintptr_t paramOff) {
    std::string line = "Equip snapshot idx=";
    const uintptr_t eq = pgd + kEquipGameData;
    for (int i = 0; i < kEquipSlotCount; ++i) {
        int idx = 0;
        mem::Read(eq + kEquipIdxList + static_cast<uintptr_t>(i) * 4, idx);
        if (i) line += ",";
        line += std::to_string(idx);
    }
    line += " param=";
    for (int i = 0; i < kEquipSlotCount; ++i) {
        int param = 0;
        mem::Read(eq + paramOff + static_cast<uintptr_t>(i) * 4, param);
        if (i) line += ",";
        line += std::to_string(param);
    }
    log::Info(line);
}

struct InvView {
    uintptr_t obj = 0;
    uintptr_t list = 0;  // entries of stride 0x18
    int count = 0;
    int tail = 0;
};

InvView ResolveInventory(uintptr_t pgd) {
    InvView v{};
    auto try_layout = [](uintptr_t obj, bool is_pointer_obj, InvView& out) -> bool {
        uintptr_t base = obj;
        if (!base) return false;
        uintptr_t list = 0;
        int num = 0;
        int tail = 0;
        if (!mem::ReadPtr(base + 0x10, list) || !list) return false;
        if (!mem::Read(base + 0x18, num) || num < 1 || num > 3000) return false;
        mem::Read(base + 0x1C, tail);
        out.obj = is_pointer_obj ? obj : base;
        out.list = list;
        out.count = num;
        out.tail = tail;
        return true;
    };

    uintptr_t ptr = 0;
    if (mem::ReadPtr(pgd + kInvPtr, ptr) && ptr) {
        if (try_layout(ptr, true, v)) return v;
    }
    if (try_layout(pgd + kInvInline, false, v)) return v;
    return {};
}

int FindItemIdx(const InvView& inv, unsigned want_id) {
    if (!inv.list || inv.count < 1) return -1;
    const unsigned want = StripType(want_id);
    int seen = 0;
    const int cap = std::min(inv.count + 64, 2688);
    for (int i = 0; i < cap && seen < inv.count; ++i) {
        unsigned id = 0xFFFFFFFFu;
        if (!mem::Read(inv.list + static_cast<uintptr_t>(i) * 0x18 + 4, id)) break;
        if (id == 0xFFFFFFFFu) continue;
        ++seen;
        if (id == want_id || StripType(id) == want) return i;
    }
    return -1;
}

int DummyForSlot(int slot) {
    if (slot >= 0 && slot <= 5) return kUnarmedWep;
    if (slot >= 12 && slot <= 15) return kNakedArmor[slot - 12];
    return kEmptyId;
}

using EquipGearFn = void (*)(void* eq, unsigned slot, int* item, int idx, int a, int b, int c);

bool InvokeEquipGearSeh(uintptr_t fn, void* eq, unsigned slot, int* item, int idx) {
    __try {
        auto call = reinterpret_cast<EquipGearFn>(fn);
        call(eq, slot, item, idx, 1, 1, 0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

uintptr_t ResolveEquipGear() {
    static uintptr_t fn = 0;
    static bool resolved = false;
    if (resolved) return fn;
    resolved = true;
    const char* pats[] = {
        "48 8B F1 48 8B D8 48 63 EA 48 8B F9",
        "?? 8B F1 ?? 8B D8 ?? 63 EA ?? 8B F9",
    };
    for (const char* pat : pats) {
        auto unique = mem::FindUniqueAob(pat);
        if (!unique) unique = mem::FindAob(pat);
        if (!unique) continue;
        fn = *unique - 0x17;
        log::Info("equipGear fn=0x" + HexU(fn));
        return fn;
    }
    log::Error("equipGear AOB not found");
    return 0;
}

using ItemGiveFn = void (*)(void* map_item_man, void* request, void* scratch, unsigned unk);

struct SpawnRequest {
    unsigned count;
    unsigned id;
    unsigned qty;
    int dur;
    int gem;
};

bool InvokeItemGiveSeh(uintptr_t fn, uintptr_t map, SpawnRequest* req, void* scratch) {
    __try {
        auto call = reinterpret_cast<ItemGiveFn>(fn);
        call(reinterpret_cast<void*>(map), req, scratch, 0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool CallItemGive(unsigned item_id, unsigned qty) {
    static std::mutex mu;
    std::lock_guard lock(mu);

    static uintptr_t fn = 0;
    static uintptr_t map_slot = 0;
    static bool resolved = false;
    if (!resolved) {
        resolved = true;
        if (auto hit = mem::FindAob(
                "48 8B C4 48 89 58 08 48 89 68 10 48 89 70 18 48 89 78 20 41 56 48 83 EC ?? 8B 02 83 F8 0A")) {
            fn = *hit;
        } else if (auto short_hit = mem::FindUniqueAob("8B 02 83 F8 0A")) {
            fn = *short_hit - 0x52;
        }
        const char* map_aobs[] = {
            "48 8B 0D ?? ?? ?? ?? C7 44 24 50 FF FF FF FF",
            "48 8B 0D ?? ?? ?? ?? 48 85 C9 0F 84 ?? ?? ?? ?? 48 8D 54 24",
            "48 8B 0D ?? ?? ?? ?? 48 85 C9 74 ?? 8B 49 28",
        };
        for (const char* pat : map_aobs) {
            auto hit = mem::FindAob(pat);
            if (!hit) continue;
            auto slot = mem::ResolveRip(*hit, 3, 7);
            if (!slot) continue;
            uintptr_t obj = 0;
            if (mem::ReadPtr(*slot, obj) && obj) {
                map_slot = *slot;
                break;
            }
        }
        if (fn && map_slot) {
            log::Info("ItemGive fn=0x" + HexU(fn) + " MapItemMan=0x" + HexU(map_slot));
        }
    }
    if (!fn || !map_slot) {
        log::Error("ItemGive: function or MapItemMan not found");
        return false;
    }
    uintptr_t map = 0;
    if (!mem::ReadPtr(map_slot, map) || !map) {
        log::Error("ItemGive: MapItemMan null");
        return false;
    }

    SpawnRequest req{1, item_id, qty, -1, -1};
    alignas(16) unsigned char scratch[128];
    std::memset(scratch, 0, sizeof(scratch));
    if (!InvokeItemGiveSeh(fn, map, &req, scratch)) {
        log::Error("ItemGive: call crashed (SEH)");
        return false;
    }
    return true;
}

bool UnequipViaGame(uintptr_t pgd, int slot) {
    const uintptr_t gear = ResolveEquipGear();
    if (!gear) return false;
    auto inv = ResolveInventory(pgd);
    int dummy = DummyForSlot(slot);
    int inv_idx = kEmptyId;
    if (dummy != kEmptyId) {
        inv_idx = FindItemIdx(inv, static_cast<unsigned>(dummy));
        if (inv_idx < 0) {
            CallItemGive(static_cast<unsigned>(dummy), 1);
            inv = ResolveInventory(pgd);
            inv_idx = FindItemIdx(inv, static_cast<unsigned>(dummy));
        }
        if (inv_idx < 0) {
            log::Warn("Unequip: dummy item " + std::to_string(dummy) + " not in inventory");
            return false;
        }
    }
    int item = dummy;
    const int call_idx = (inv_idx == kEmptyId) ? kEmptyId : (inv_idx + inv.tail);
    void* eq = reinterpret_cast<void*>(pgd + kEquipGameData);
    if (!InvokeEquipGearSeh(gear, eq, static_cast<unsigned>(slot), &item, call_idx)) {
        log::Error("equipGear crashed (SEH) slot=" + std::to_string(slot));
        return false;
    }
    return true;
}

bool UnequipRandomOnPgd(uintptr_t pgd) {
    const uintptr_t paramOff = ParamArrayOffset(pgd);
    LogEquipSnapshot(pgd, paramOff);
    const int chaos_slots[] = {0, 1, 2, 3, 4, 5, 12, 13, 14, 15, 17, 18, 19, 20};
    std::vector<int> filled;
    for (int slot : chaos_slots) {
        int idx = 0, param = 0;
        if (!ReadEquipSlot(pgd, paramOff, slot, idx, param)) continue;
        if (SlotHasGear(slot, param)) filled.push_back(slot);
    }
    if (filled.empty()) {
        log::Warn("Unequip: no filled gear slots");
        return false;
    }
    static bool seeded = false;
    if (!seeded) {
        std::srand(static_cast<unsigned>(std::time(nullptr)) ^ 0xE911u);
        seeded = true;
    }
    for (size_t i = filled.size() - 1; i > 0; --i) {
        const size_t j = static_cast<size_t>(std::rand()) % (i + 1);
        std::swap(filled[i], filled[j]);
    }
    std::string cand = "Unequip candidates";
    for (int slot : filled) {
        int idx = 0, param = 0;
        ReadEquipSlot(pgd, paramOff, slot, idx, param);
        cand += " " + std::to_string(slot) + "=" + std::to_string(param);
    }
    log::Info(cand);
    for (int slot : filled) {
        int idx = 0, before = 0;
        ReadEquipSlot(pgd, paramOff, slot, idx, before);
        if (!SlotHasGear(slot, before)) continue;
        if (!UnequipViaGame(pgd, slot)) continue;
        int after = before;
        ReadEquipSlot(pgd, paramOff, slot, idx, after);
        if (SlotHasGear(slot, after) && after == before) {
            log::Warn("Unequip: slot " + std::to_string(slot) + " still occupied param=" +
                      std::to_string(after) + ", trying another");
            continue;
        }
        log::Info("Unequipped slot " + std::to_string(slot) + " param=" + std::to_string(before) +
                  " -> " + std::to_string(after));
        return true;
    }
    log::Warn("Unequip: every occupied slot failed to clear");
    return false;
}

}  // namespace

bool HealHp(const Config& cfg, targets::Mode mode) {
    auto targets_list = targets::SelectTargets(cfg, mode);
    if (targets_list.empty()) {
        log::Error("Heal: no players found");
        return false;
    }
    int ok = 0;
    for (const auto& p : targets_list) {
        if (HealOne(p, cfg)) ++ok;
    }
    log::Info("Heal done " + std::to_string(ok) + "/" + std::to_string(targets_list.size()) +
              " (" + targets::DescribeTargets(targets_list) + ")");
    return ok > 0;
}

bool RestoreFp(const Config& cfg, targets::Mode mode) {
    auto targets_list = targets::SelectTargets(cfg, mode);
    if (targets_list.empty()) return false;
    int ok = 0;
    for (const auto& p : targets_list) {
        if (RestoreFpOne(p)) ++ok;
    }
    log::Info("FP done " + std::to_string(ok) + "/" + std::to_string(targets_list.size()));
    return ok > 0;
}

bool LoseRunesPercent(const Config& cfg, int percent, targets::Mode mode) {
    // Runes live in local PlayerGameData on this machine. In Seamless, each client owns
    // their own save data — applying "all" from host affects the host's GameDataMan.
    // We still honor target mode for future per-player PGD wiring; today apply once for
    // host when any selected target includes local, or always apply host PGD for all.
    auto targets_list = targets::SelectTargets(cfg, mode);
    auto pgd = LocalPlayerGameData(cfg);
    if (!pgd) return false;

    bool touch = (mode == targets::Mode::All);
    for (const auto& p : targets_list) {
        if (p.is_local) touch = true;
    }
    if (!touch && mode == targets::Mode::Random && !targets_list.empty()) {
        // Random target on host-only session still hits host PGD when alone.
        touch = targets_list[0].is_local || targets_list.size() == 1;
    }
    if (!touch) {
        log::Warn("LoseRunes: selected remote player — host cannot edit their runes; skipped");
        return false;
    }
    return LoseRunesOnPgd(*pgd, percent);
}

bool ApplyStatDelta(const Config& cfg, const std::string& stat, int delta, targets::Mode mode) {
    auto off = StatOffset(cfg, stat);
    if (!off) {
        log::Error("Unknown stat: " + stat);
        return false;
    }
    auto targets_list = targets::SelectTargets(cfg, mode);
    auto pgd = LocalPlayerGameData(cfg);
    if (!pgd) return false;

    bool touch = (mode == targets::Mode::All);
    for (const auto& p : targets_list) {
        if (p.is_local) touch = true;
    }
    if (!touch && !targets_list.empty()) touch = true;  // solo / host session
    if (!touch) return false;
    return ApplyStatOnPgd(*pgd, *off, delta, stat);
}

bool GiveRandomWeapon(const Config& cfg, targets::Mode mode) {
    (void)mode;
    (void)cfg;
    static bool seeded = false;
    if (!seeded) {
        std::srand(static_cast<unsigned>(std::time(nullptr)) ^ 0xA11u);
        seeded = true;
    }
    constexpr int n = static_cast<int>(sizeof(kRandomWeapons) / sizeof(kRandomWeapons[0]));
    const WeaponPick& pick = kRandomWeapons[std::rand() % n];
    log::Info(std::string("Give weapon: ") + pick.name + " id=" + std::to_string(pick.id) +
              " pool=" + std::to_string(n));
    return CallItemGive(pick.id, 1);
}

bool UnequipRandomGear(const Config& cfg, targets::Mode mode) {
    (void)mode;
    auto pgd = LocalPlayerGameData(cfg);
    if (!pgd) return false;
    return UnequipRandomOnPgd(*pgd);
}

constexpr unsigned GoodsId(unsigned id) { return 0x40000000u | id; }
constexpr unsigned ArmorId(unsigned id) { return 0x10000000u | id; }

struct LootItem {
    unsigned id;
    const char* name;
};

struct BossLoot {
    const char* label;
    int runes;
    const LootItem* items;
    int n_items;
};

const BossLoot* LootForKey(std::string key) {
    key = Lower(std::move(key));

    static const LootItem kGodrick[] = {
        {GoodsId(2950), "Remembrance of the Grafted"},
        {GoodsId(191), "Godrick's Great Rune"},
    };
    static const LootItem kRennala[] = {
        {GoodsId(2959), "Remembrance of the Full Moon Queen"},
        {GoodsId(10080), "Great Rune of the Unborn"},
    };
    static const LootItem kGideon[] = {
        {11110000u, "Scepter of the All-Knowing"},
        {ArmorId(590000), "All-Knowing Helm"},
        {ArmorId(590100), "All-Knowing Armor"},
        {ArmorId(590200), "All-Knowing Gauntlets"},
        {ArmorId(590300), "All-Knowing Greaves"},
    };
    static const LootItem kMaliketh[] = {{GoodsId(2956), "Remembrance of the Black Blade"}};
    static const LootItem kFireGiant[] = {{GoodsId(2961), "Remembrance of the Fire Giant"}};
    static const LootItem kHoarah[] = {{GoodsId(2957), "Remembrance of Hoarah Loux"}};
    static const LootItem kElden[] = {{GoodsId(2963), "Elden Remembrance"}};

    static const BossLoot kMargit{"Margit", 12000, nullptr, 0};
    static const BossLoot kGodrickLoot{"Godrick", 20000, kGodrick, 2};
    static const BossLoot kWolf{"Red Wolf of Radagon", 14000, nullptr, 0};
    static const BossLoot kRennalaLoot{"Rennala", 40000, kRennala, 2};
    static const BossLoot kShade{"Godfrey (golden shade)", 80000, nullptr, 0};
    static const BossLoot kGideonLoot{"Sir Gideon", 150000, kGideon, 5};
    static const BossLoot kDuo{"Godskin Duo", 170000, nullptr, 0};
    static const BossLoot kMalikethLoot{"Maliketh", 240000, kMaliketh, 1};
    static const BossLoot kGiantLoot{"Fire Giant", 180000, kFireGiant, 1};
    static const BossLoot kHoarahLoot{"Hoarah Loux", 300000, kHoarah, 1};
    static const BossLoot kBeastLoot{"Elden Beast", 500000, kElden, 1};

    if (key == "boss_margit" || key == "margit") return &kMargit;
    if (key == "boss_godrick" || key == "godrick") return &kGodrickLoot;
    if (key == "boss_red_wolf" || key == "red_wolf") return &kWolf;
    if (key == "boss_rennala" || key == "renala" || key == "rennala") return &kRennalaLoot;
    if (key == "boss_godfrey_shade" || key == "godfrey_shade") return &kShade;
    if (key == "boss_gideon" || key == "gideon") return &kGideonLoot;
    if (key == "boss_godskin" || key == "godskin_duo" || key == "godskin") return &kDuo;
    if (key == "boss_maliketh" || key == "maliketh") return &kMalikethLoot;
    if (key == "boss_fire_giant" || key == "fire_giant") return &kGiantLoot;
    if (key == "boss_godfrey" || key == "godfrey" || key == "hoarah") return &kHoarahLoot;
    if (key == "boss_elden_beast" || key == "elden_beast") return &kBeastLoot;
    return nullptr;
}

std::mutex g_souls_mu;

bool AddRunesOnPgdLocked(uintptr_t pgd, int amount) {
    if (amount <= 0) return true;
    int souls = 0;
    if (!mem::Read(pgd + kSoulsOff, souls) || souls < 0) souls = 0;
    long long next = static_cast<long long>(souls) + amount;
    if (next > 999999999LL) next = 999999999LL;
    if (!mem::Write(pgd + kSoulsOff, static_cast<int>(next))) return false;
    log::Info("Runes: " + std::to_string(souls) + " -> " + std::to_string(static_cast<int>(next)) +
              " (+" + std::to_string(amount) + ")");
    return true;
}

bool HasBossDrops(const std::string& enemy_key) { return LootForKey(enemy_key) != nullptr; }

int GiveBossKillItems(const std::string& enemy_key) {
    const BossLoot* loot = LootForKey(enemy_key);
    if (!loot) return -1;
    int given = 0;
    for (int i = 0; i < loot->n_items; ++i) {
        const LootItem& it = loot->items[i];
        if (CallItemGive(it.id, 1)) {
            ++given;
            log::Info(std::string("Boss drop: ") + loot->label + " -> " + it.name);
        } else {
            log::Warn(std::string("Boss drop failed: ") + loot->label + " -> " + it.name);
        }
    }
    if (loot->n_items == 0) {
        log::Info(std::string("Boss drop: ") + loot->label + " (runes only)");
    } else if (given == 0) {
        log::Warn(std::string("Boss drop: ") + loot->label + " gave no items");
    }
    return loot->runes;
}

int ReadRunes(const Config& cfg) {
    auto pgd = LocalPlayerGameData(cfg);
    if (!pgd) return -1;
    std::lock_guard lock(g_souls_mu);
    int souls = 0;
    if (!mem::Read(*pgd + kSoulsOff, souls) || souls < 0) return -1;
    return souls;
}

bool AddRunes(const Config& cfg, int amount) {
    if (amount <= 0) return true;
    auto pgd = LocalPlayerGameData(cfg);
    if (!pgd) return false;
    std::lock_guard lock(g_souls_mu);
    return AddRunesOnPgdLocked(*pgd, amount);
}

int ReadSoulLevel(const Config& cfg) {
    auto pgd = LocalPlayerGameData(cfg);
    if (!pgd) return 0;
    int level = 0;
    if (mem::Read(*pgd + 0x68, level) && level >= 1 && level <= 713) return level;
    const uintptr_t offs[] = {
        cfg.stats.vigor, cfg.stats.mind, cfg.stats.endurance, cfg.stats.strength,
        cfg.stats.dexterity, cfg.stats.intelligence, cfg.stats.faith, cfg.stats.arcane,
    };
    int sum = 0;
    for (uintptr_t off : offs) {
        int s = 0;
        if (!mem::Read(*pgd + off, s) || s < 1 || s > 99) return 0;
        sum += s;
    }
    const int computed = sum - 79;
    if (computed >= 1 && computed <= 713) return computed;
    return 0;
}

int ReadLocalMaxHp(const Config& cfg) {
    auto list = targets::CollectPlayers(cfg);
    for (const auto& p : list) {
        if (!p.is_local || !p.char_data) continue;
        int mx = 0;
        if (mem::Read(p.char_data + cfg.hp_max, mx) && mx >= 200 && mx <= 50000) return mx;
        if (mem::Read(p.char_data + 0x13C, mx) && mx >= 200 && mx <= 50000) return mx;
    }
    for (const auto& p : list) {
        if (!p.char_data) continue;
        int mx = 0;
        if (mem::Read(p.char_data + cfg.hp_max, mx) && mx >= 200 && mx <= 50000) return mx;
    }
    return 0;
}

}  // namespace chaos::player
