#include "game/player.hpp"
#include "game/memory.hpp"
#include "game/params.hpp"
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

constexpr uintptr_t kInvEntryStride = 0x18;

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

// item points at an inventory entry: {gaitem handle, item id, qty, ...}. The game copies
// the handle into ChrAsm, so a bare item id leaves weapons without an instance (invisible).
using EquipGearFn = void (*)(void* eq, unsigned slot, void* item, int idx, int a, int b, int c);

struct InvEntry {
    uint32_t handle;
    uint32_t id;
    uint32_t qty;
    uint32_t sort;
    uint32_t pad[2];
};

bool ReadInvEntry(const InvView& inv, int normal_idx, InvEntry& out) {
    if (!inv.list || normal_idx < 0 || normal_idx >= 3072) return false;
    return mem::Read(inv.list + static_cast<uintptr_t>(normal_idx) * kInvEntryStride, out) &&
           out.id != 0xFFFFFFFFu;
}

bool InvokeEquipGearSeh(uintptr_t fn, void* eq, unsigned slot, void* item, int idx) {
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
    InvEntry item{};
    item.handle = static_cast<uint32_t>(dummy);
    item.id = static_cast<uint32_t>(dummy);
    if (inv_idx != kEmptyId) ReadInvEntry(inv, inv_idx, item);
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

// ---- equipment chaos -------------------------------------------------------

// EquipInventoryData lists (fromsoftware-rs InventoryItemsData). Flasks live in the
// normal list on live ER; the key list is scanned too in case that changes.
constexpr uintptr_t kInvNormalHead = 0x10;
constexpr uintptr_t kInvKeyHead = 0x20;
constexpr uintptr_t kInvKeyLen = 0x28;
constexpr uintptr_t kInvEntrySize = 0x18;
constexpr uintptr_t kMaxCrimsonFlasks = 0x101;
constexpr uintptr_t kMaxCeruleanFlasks = 0x102;

// CSWepGaitemIns: gem slot handle at +0x28. CSGemGaitemIns: owning weapon at +0x10.
constexpr uintptr_t kGaitemHandle = 0x08;
constexpr uintptr_t kGaitemItemId = 0x0C;
constexpr uintptr_t kWepGemHandle = 0x28;
constexpr uintptr_t kGemWeaponHandle = 0x10;
constexpr unsigned kGemType = 0x80000000u;

// EquipParamWeapon / EquipParamGem field offsets (Paramdex ER defs).
constexpr uintptr_t kWepType = 0x1A6;
constexpr uintptr_t kWepGemMountType = 0x241;
constexpr uintptr_t kGemAffinityBits = 0x30;
constexpr uintptr_t kGemMountBits = 0x38;

// ChrAsm around the param id array: gaitem handles sit right before it, and the
// selected left/right weapon slot (0..2) before those.
struct HandLayout {
    uintptr_t params;
    uintptr_t handles;
    uintptr_t left_sel;
    uintptr_t right_sel;
};

HandLayout HandsOf(uintptr_t pgd) {
    const uintptr_t p = ParamArrayOffset(pgd);
    const uintptr_t h = p - kEquipSlotCount * 4;
    return {p, h, h - 0x18, h - 0x14};
}

bool ActiveWeaponSlots(uintptr_t pgd, const HandLayout& h, int& left, int& right) {
    const uintptr_t eq = pgd + kEquipGameData;
    uint32_t l = 99, r = 99;
    if (!mem::Read(eq + h.left_sel, l) || !mem::Read(eq + h.right_sel, r) || l > 2 || r > 2) {
        log::Warn("Active weapon slots unreadable l=" + std::to_string(l) + " r=" + std::to_string(r));
        return false;
    }
    left = static_cast<int>(l) * 2;
    right = static_cast<int>(r) * 2 + 1;
    return true;
}

int ReadSlotInt(uintptr_t pgd, uintptr_t off, int slot) {
    int v = kEmptyId;
    mem::Read(pgd + kEquipGameData + off + static_cast<uintptr_t>(slot) * 4, v);
    return v;
}

// Inventory entry behind an equip-list index (normal list, offset by the key capacity).
// Falls back to a scan by item id when the index no longer points at that item.
bool EntryForEquipIdx(const InvView& inv, int equip_idx, int param, InvEntry& out, int& out_idx) {
    const unsigned want = StripType(static_cast<unsigned>(param));
    const int n = equip_idx - inv.tail;
    if (ReadInvEntry(inv, n, out) && StripType(out.id) == want) {
        out_idx = equip_idx;
        return true;
    }
    const int found = FindItemIdx(inv, static_cast<unsigned>(param));
    if (found >= 0 && ReadInvEntry(inv, found, out)) {
        out_idx = found + inv.tail;
        return true;
    }
    return false;
}

bool EquipInto(uintptr_t pgd, int slot, int param, int idx) {
    const uintptr_t gear = ResolveEquipGear();
    if (!gear) return false;
    const InvView inv = ResolveInventory(pgd);
    InvEntry item{};
    int call_idx = idx;
    if (!EntryForEquipIdx(inv, idx, param, item, call_idx)) {
        log::Warn("Equip: item " + std::to_string(param) + " idx " + std::to_string(idx) +
                  " not in inventory");
        return false;
    }
    void* eq = reinterpret_cast<void*>(pgd + kEquipGameData);
    if (!InvokeEquipGearSeh(gear, eq, static_cast<unsigned>(slot), &item, call_idx)) {
        log::Error("equipGear crashed (SEH) slot=" + std::to_string(slot));
        return false;
    }
    return true;
}

uintptr_t g_gaitem_slot = 0;
int g_gaitem_tries = 0;

uintptr_t GaitemIns(uint32_t handle) {
    if (!(handle & (1u << 23))) return 0;
    const uintptr_t idx = handle & 0xFFFFu;
    if (idx >= 5120) return 0;
    if (!g_gaitem_slot && g_gaitem_tries < 3) {
        ++g_gaitem_tries;
        g_gaitem_slot = params::FindSingletonSlot("CSGaitemImp", [handle, idx](uintptr_t obj) {
            uintptr_t ins = 0;
            uint32_t h = 0;
            return mem::ReadPtr(obj + 8 + idx * 8, ins) && ins && mem::Read(ins + kGaitemHandle, h) &&
                   h == handle;
        });
        if (g_gaitem_slot) log::Info("CSGaitemImp slot=base+0x" + HexU(g_gaitem_slot - mem::ModuleBase()));
        else log::Error("CSGaitemImp not found");
    }
    if (!g_gaitem_slot) return 0;
    uintptr_t imp = 0, ins = 0;
    uint32_t h = 0;
    if (!mem::ReadPtr(g_gaitem_slot, imp) || !imp) return 0;
    if (!mem::ReadPtr(imp + 8 + idx * 8, ins) || !ins) return 0;
    if (!mem::Read(ins + kGaitemHandle, h) || h != handle) return 0;
    return ins;
}

// EquipParamWeapon.wepType -> bit in EquipParamGem.canMountWep_* (0x38..0x3C).
int MountBitForWepType(int t) {
    switch (t) {
        case 1: return 0;    // dagger
        case 3: return 1;    // straight sword
        case 5: return 2;    // greatsword
        case 7: return 3;    // colossal sword
        case 9: return 4;    // curved sword
        case 11: return 5;   // curved greatsword
        case 13: return 6;   // katana
        case 14: return 7;   // twinblade
        case 15: return 8;   // thrusting sword
        case 16: return 9;   // heavy thrusting sword
        case 17: return 10;  // axe
        case 19: return 11;  // greataxe
        case 21: return 12;  // hammer
        case 23: return 13;  // great hammer
        case 24: return 14;  // flail
        case 25: return 15;  // spear
        case 28: return 16;  // great spear
        case 29: return 18;  // halberd
        case 31: return 19;  // reaper
        case 35: return 20;  // fist
        case 37: return 21;  // claw
        case 39: return 22;  // whip
        case 41: return 23;  // colossal weapon
        case 50: return 24;  // light bow
        case 51: return 25;  // bow
        case 53: return 26;  // greatbow
        case 55: return 27;  // crossbow
        case 56: return 28;  // ballista
        case 65: return 32;  // small shield
        case 67: return 33;  // medium shield
        case 69: return 34;  // greatshield
        case 87: return 35;  // torch
        default: return -1;
    }
}

bool IsPlayerAsh(uint32_t id) {
    if (id % 100 != 0) return false;
    if (id >= 10000 && id <= 85000) return id != 30900;  // 30900 = "No Skill"
    return (id >= 400000 && id <= 422000) || id == 800000;
}

bool RandomizeAshOnPgd(uintptr_t pgd) {
    const HandLayout h = HandsOf(pgd);
    int left = 0, right = 0;
    if (!ActiveWeaponSlots(pgd, h, left, right)) return false;
    const uintptr_t eq = pgd + kEquipGameData;

    for (int slot : {right, left}) {
        const int param = ReadSlotInt(pgd, h.params, slot);
        if (!ParamLooksEquipped(slot, param)) continue;
        const uint32_t base_id = static_cast<uint32_t>(param) / 100u * 100u;
        const uintptr_t wrow = params::Row(params::Table::Weapon, base_id);
        if (!wrow) {
            log::Warn("Ash of War: no weapon param for " + std::to_string(param));
            continue;
        }
        uint8_t mount = 0;
        uint16_t wtype = 0;
        mem::Read(wrow + kWepGemMountType, mount);
        mem::Read(wrow + kWepType, wtype);
        if (mount != 2) {
            log::Info("Ash of War: slot " + std::to_string(slot) + " weapon " + std::to_string(param) +
                      " has a fixed skill");
            continue;
        }
        const int bit = MountBitForWepType(wtype);
        if (bit < 0) {
            log::Warn("Ash of War: weapon type " + std::to_string(wtype) + " not mapped");
            continue;
        }
        // ChrAsm handles can be stale (older builds wrote item ids there); the inventory
        // entry always carries the real one.
        uint32_t wep_handle = static_cast<uint32_t>(ReadSlotInt(pgd, h.handles, slot));
        const int wep_idx = ReadSlotInt(pgd, kEquipIdxList, slot);
        if (!GaitemIns(wep_handle)) {
            InvEntry ent{};
            int ignored = 0;
            if (EntryForEquipIdx(ResolveInventory(pgd), wep_idx, param, ent, ignored)) wep_handle = ent.handle;
        }
        const uintptr_t wep = GaitemIns(wep_handle);
        if (!wep || !params::RttiNameContains(wep, "WepGaitemIns")) {
            log::Warn("Ash of War: weapon instance not found handle=0x" + HexU(wep_handle));
            continue;
        }
        uint32_t old_gem_handle = 0;
        mem::Read(wep + kWepGemHandle, old_gem_handle);
        const uintptr_t old_gem = GaitemIns(old_gem_handle);
        uint32_t old_gem_item = 0;
        if (old_gem) mem::Read(old_gem + kGaitemItemId, old_gem_item);

        const int affinity = (param % 10000) / 100;
        std::vector<uint32_t> keeps_affinity, fits;
        int ashes_seen = 0;
        std::string sample;
        params::ForEachRow(params::Table::Gem, [&](uint32_t id, uintptr_t row) {
            if (!IsPlayerAsh(id) || (kGemType | id) == old_gem_item) return;
            uint8_t mounts[5]{};
            if (!mem::ReadBytes(row + kGemMountBits, mounts, sizeof(mounts))) return;
            if (ashes_seen++ == 0) {
                sample = std::to_string(id) + " mounts=";
                for (uint8_t b : mounts) sample += HexU(b) + " ";
            }
            if (!((mounts[bit / 8] >> (bit % 8)) & 1)) return;
            fits.push_back(id);
            uint8_t attrs[2]{};
            if (affinity < 16 && mem::ReadBytes(row + kGemAffinityBits, attrs, sizeof(attrs)) &&
                ((attrs[affinity / 8] >> (affinity % 8)) & 1)) {
                keeps_affinity.push_back(id);
            }
        });
        const auto& pool = keeps_affinity.empty() ? fits : keeps_affinity;
        if (pool.empty()) {
            log::Warn("Ash of War: no ash fits weapon " + std::to_string(param) + " type " +
                      std::to_string(wtype) + " bit " + std::to_string(bit) + " (ashes seen " +
                      std::to_string(ashes_seen) + ", first " + sample + ")");
            continue;
        }
        const uint32_t gem_id = pool[static_cast<size_t>(std::rand()) % pool.size()];
        const unsigned gem_item = kGemType | gem_id;
        if (!CallItemGive(gem_item, 1)) continue;

        // The newest copy of that ash in the inventory is the one we just got.
        const InvView inv = ResolveInventory(pgd);
        uint32_t gem_handle = 0, best_sort = 0;
        for (int i = 0; i < std::min(inv.count + 64, 3000); ++i) {
            const uintptr_t ent = inv.list + static_cast<uintptr_t>(i) * kInvEntrySize;
            uint32_t id = 0, gh = 0, sort = 0;
            if (!mem::Read(ent + 4, id)) break;
            if (id != gem_item) continue;
            mem::Read(ent, gh);
            mem::Read(ent + 0xC, sort);
            if (!gem_handle || sort >= best_sort) {
                gem_handle = gh;
                best_sort = sort;
            }
        }
        const uintptr_t gem = GaitemIns(gem_handle);
        if (!gem || !params::RttiNameContains(gem, "GemGaitemIns")) {
            log::Warn("Ash of War: new ash " + std::to_string(gem_id) + " not found in inventory");
            continue;
        }
        uint32_t unattached = 0;
        mem::Read(gem + kGemWeaponHandle, unattached);
        mem::Write(gem + kGemWeaponHandle, wep_handle);
        mem::Write(wep + kWepGemHandle, gem_handle);
        if (old_gem) mem::Write(old_gem + kGemWeaponHandle, unattached);

        // Re-equip so the game rebuilds the skill and model from the new ash.
        UnequipViaGame(pgd, slot);
        if (!EquipInto(pgd, slot, param, wep_idx)) {
            log::Error("Ash of War: re-equip failed, weapon left in inventory");
        }
        log::Info("Ash of War: slot " + std::to_string(slot) + " weapon " + std::to_string(param) +
                  " ash " + std::to_string(old_gem_item & 0x0FFFFFFFu) + " -> " + std::to_string(gem_id) +
                  " (pool " + std::to_string(pool.size()) + ")");
        return true;
    }
    log::Warn("Ash of War: no equipped weapon can take an ash");
    return false;
}

bool SwapHandsOnPgd(uintptr_t pgd) {
    const HandLayout h = HandsOf(pgd);
    int left = 0, right = 0;
    if (!ActiveWeaponSlots(pgd, h, left, right)) return false;
    const int pl = ReadSlotInt(pgd, h.params, left);
    const int pr = ReadSlotInt(pgd, h.params, right);
    const int il = ReadSlotInt(pgd, kEquipIdxList, left);
    const int ir = ReadSlotInt(pgd, kEquipIdxList, right);
    const bool has_l = ParamLooksEquipped(left, pl);
    const bool has_r = ParamLooksEquipped(right, pr);
    if (!has_l && !has_r) {
        log::Warn("Swap hands: both hands empty");
        return false;
    }
    // Free both slots first so neither item is equipped twice mid-swap.
    if (has_r) UnequipViaGame(pgd, right);
    if (has_l) UnequipViaGame(pgd, left);
    if (has_r) EquipInto(pgd, left, pr, ir);
    if (has_l) EquipInto(pgd, right, pl, il);
    log::Info("Swap hands: left " + std::to_string(pl) + " <-> right " + std::to_string(pr) + " now L=" +
              std::to_string(ReadSlotInt(pgd, h.params, left)) + " R=" +
              std::to_string(ReadSlotInt(pgd, h.params, right)));
    return true;
}

// Goods use unindexed gaitem handles that embed the item id (category 3 + flag bit).
constexpr uint32_t kGoodsHandle = 0xB0000000u;
constexpr uint32_t kGoodsItem = 0x40000000u;

struct FlaskSwap {
    uint32_t old_id, new_id, old_handle, new_handle;
};

// Crimson 1000-1025 / Cerulean 1050-1075: odd id = filled flask, even = empty.
// An empty flask keeps quantity 1; quantity 0 deletes it from the quick slots.
int EditFlasks(uintptr_t head, uint32_t len, bool fill, int crimson_max, int cerulean_max,
               std::vector<FlaskSwap>& swaps) {
    if (!head || len == 0 || len > 3000) return 0;
    int n = 0;
    for (uint32_t i = 0; i < len; ++i) {
        const uintptr_t ent = head + static_cast<uintptr_t>(i) * kInvEntrySize;
        uint32_t id = 0, qty = 0, handle = 0;
        if (!mem::Read(ent + 4, id) || id == 0xFFFFFFFFu) continue;
        if ((id >> 28) != 4) continue;  // goods only; talismans reuse 1000+ ids
        const uint32_t raw = id & 0x0FFFFFFFu;
        const bool crimson = raw >= 1000 && raw <= 1025;
        const bool cerulean = raw >= 1050 && raw <= 1075;
        if (!crimson && !cerulean) continue;
        mem::Read(ent, handle);
        mem::Read(ent + 8, qty);
        const uint32_t next_raw = fill ? (raw | 1u) : (raw & ~1u);
        const uint32_t next_id = kGoodsItem | next_raw;
        const uint32_t next_handle = kGoodsHandle | next_raw;
        const int max = crimson ? crimson_max : cerulean_max;
        const uint32_t next_qty = fill ? static_cast<uint32_t>(std::max(max, 1)) : 1u;
        mem::Write(ent, next_handle);
        mem::Write(ent + 4, next_id);
        mem::Write(ent + 8, next_qty);
        if (next_id != id) swaps.push_back({id, next_id, handle, next_handle});
        log::Info(std::string(fill ? "Refill " : "Drain ") + (crimson ? "crimson" : "cerulean") + " id " +
                  std::to_string(raw) + " x" + std::to_string(qty) + " handle 0x" + HexU(handle) + " -> " +
                  std::to_string(next_raw) + " x" + std::to_string(next_qty));
        ++n;
    }
    return n;
}

// Quick slots / pouch store the gaitem handle and ChrAsmEquipEntries stores the item id;
// both live inside EquipGameData and must follow the flask's new id.
int RepointFlaskRefs(uintptr_t pgd, const std::vector<FlaskSwap>& swaps) {
    constexpr uintptr_t kScanStart = 0x60;
    constexpr uintptr_t kScanEnd = 0x600;
    const uintptr_t eq = pgd + kEquipGameData;
    int n = 0;
    for (uintptr_t off = kScanStart; off < kScanEnd; off += 4) {
        uint32_t v = 0;
        if (!mem::Read(eq + off, v)) continue;
        for (const FlaskSwap& s : swaps) {
            uint32_t to = 0;
            if (v == s.old_handle && (s.old_handle >> 28) == 0xB) to = s.new_handle;
            else if (v == s.old_id) to = s.new_id;
            else continue;
            mem::Write(eq + off, to);
            log::Info("Flasks: EquipGameData+0x" + HexU(off) + " 0x" + HexU(v) + " -> 0x" + HexU(to));
            ++n;
            break;
        }
    }
    return n;
}

bool SetFlasksOnPgd(uintptr_t pgd, bool fill) {
    const InvView inv = ResolveInventory(pgd);
    if (!inv.obj) {
        log::Warn("Flasks: inventory not found");
        return false;
    }
    uint8_t crimson = 0, cerulean = 0;
    mem::Read(pgd + kMaxCrimsonFlasks, crimson);
    mem::Read(pgd + kMaxCeruleanFlasks, cerulean);
    if (crimson + cerulean > 20) {
        log::Warn("Flasks: max counts look wrong (" + std::to_string(crimson) + "/" +
                  std::to_string(cerulean) + ")");
        return false;
    }
    // Counts track live entries, not the highest used index, so scan to capacity.
    struct List {
        const char* name;
        uintptr_t head, len, cap;
    };
    const List lists[] = {
        {"normal", kInvNormalHead, kInvNormalHead + 8, kInvNormalHead - 8},
        {"key", kInvKeyHead, kInvKeyLen, kInvKeyHead - 4},
    };
    int n = 0;
    std::vector<FlaskSwap> swaps;
    for (const List& l : lists) {
        uintptr_t head = 0;
        uint32_t len = 0, cap = 0;
        if (!mem::ReadPtr(inv.obj + l.head, head) || !head) continue;
        mem::Read(inv.obj + l.len, len);
        mem::Read(inv.obj + l.cap, cap);
        if (cap > 4096) continue;
        n += EditFlasks(head, std::min<uint32_t>(std::max(len, cap), 3000), fill, crimson, cerulean, swaps);
    }
    if (!swaps.empty()) {
        const int refs = RepointFlaskRefs(pgd, swaps);
        log::Info("Flasks: repointed " + std::to_string(refs) + " slot references");
    }
    log::Info(std::string("Flasks ") + (fill ? "refilled" : "drained") + ": " + std::to_string(n) +
              " entries (max " + std::to_string(crimson) + " crimson / " + std::to_string(cerulean) +
              " cerulean)");
    return n > 0;
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
    static const LootItem kHoarah[] = {{GoodsId(2957), "Remembrance of Hoarah Loux"}};
    static const LootItem kMalenia[] = {
        {GoodsId(2954), "Remembrance of the Rot Goddess"},
        {GoodsId(196), "Malenia's Great Rune"},
    };
    static const LootItem kElden[] = {{GoodsId(2963), "Elden Remembrance"}};
    static const LootItem kFireGiant[] = {{GoodsId(2961), "Remembrance of the Fire Giant"}};
    static const LootItem kSentinel[] = {{18080000u, "Golden Halberd"}};

    static const BossLoot kMargit{"Margit", 12000, nullptr, 0};
    static const BossLoot kGodrickLoot{"Godrick", 20000, kGodrick, 2};
    static const BossLoot kWolf{"Red Wolf of Radagon", 14000, nullptr, 0};
    static const BossLoot kRennalaLoot{"Rennala", 40000, kRennala, 2};
    static const BossLoot kShade{"Godfrey (golden shade)", 80000, nullptr, 0};
    static const BossLoot kGideonLoot{"Sir Gideon", 150000, kGideon, 5};
    static const BossLoot kDuo{"Godskin Duo", 170000, nullptr, 0};
    static const BossLoot kMalikethLoot{"Maliketh", 240000, kMaliketh, 1};
    static const BossLoot kHoarahLoot{"Godfrey", 300000, kHoarah, 1};
    static const BossLoot kMaleniaLoot{"Malenia", 480000, kMalenia, 2};
    static const BossLoot kBeastLoot{"Elden Beast", 500000, kElden, 1};
    static const BossLoot kFireGiantLoot{"Fire Giant", 180000, kFireGiant, 1};
    // Regular enemies: runes here are only a fallback when NpcParam can't be read.
    static const BossLoot kSentinelLoot{"Tree Sentinel", 3200, kSentinel, 1};
    static const BossLoot kDogs{"Wolf", 60, nullptr, 0};
    static const BossLoot kRats{"Rat", 40, nullptr, 0};
    static const BossLoot kHawks{"Warhawk", 90, nullptr, 0};
    static const BossLoot kLobster{"Giant Lobster", 1100, nullptr, 0};
    static const BossLoot kCrabs{"Crab", 300, nullptr, 0};
    static const BossLoot kBear{"Runebear", 1400, nullptr, 0};

    if (key == "dogs") return &kDogs;
    if (key == "rats") return &kRats;
    if (key == "hawks" || key == "warhawks") return &kHawks;
    if (key == "lobster" || key == "lobsters") return &kLobster;
    if (key == "crabs" || key == "crab") return &kCrabs;
    if (key == "bear" || key == "bears" || key == "runebear") return &kBear;
    if (key == "tree_sentinel" || key == "treesentinel") return &kSentinelLoot;
    if (key == "boss_fire_giant" || key == "fire_giant") return &kFireGiantLoot;

    if (key == "boss_margit" || key == "margit") return &kMargit;
    if (key == "boss_godrick" || key == "godrick") return &kGodrickLoot;
    if (key == "boss_red_wolf" || key == "red_wolf") return &kWolf;
    if (key == "boss_rennala" || key == "renala" || key == "rennala") return &kRennalaLoot;
    if (key == "boss_godfrey_shade" || key == "godfrey_shade") return &kShade;
    if (key == "boss_gideon" || key == "gideon") return &kGideonLoot;
    if (key == "boss_godskin" || key == "godskin_duo" || key == "godskin") return &kDuo;
    if (key == "boss_maliketh" || key == "maliketh") return &kMalikethLoot;
    if (key == "boss_malenia" || key == "malenia") return &kMaleniaLoot;
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

bool GiveItem(unsigned item_id, unsigned qty) {
    return CallItemGive(item_id, qty < 1 ? 1 : qty);
}

int GiveKillLoot(const std::string& enemy_key) {
    const BossLoot* loot = LootForKey(enemy_key);
    if (!loot) return 0;
    for (int i = 0; i < loot->n_items; ++i) {
        const LootItem& it = loot->items[i];
        if (CallItemGive(it.id, 1)) {
            log::Info(std::string("Kill drop: ") + loot->label + " -> " + it.name);
        } else {
            log::Warn(std::string("Kill drop failed: ") + loot->label + " -> " + it.name);
        }
    }
    return loot->runes;
}

bool RandomizeAshOfWar(const Config& cfg) {
    auto pgd = LocalPlayerGameData(cfg);
    if (!pgd) return false;
    return RandomizeAshOnPgd(*pgd);
}

bool SwapHands(const Config& cfg) {
    auto pgd = LocalPlayerGameData(cfg);
    if (!pgd) return false;
    return SwapHandsOnPgd(*pgd);
}

bool SetFlasks(const Config& cfg, bool fill) {
    auto pgd = LocalPlayerGameData(cfg);
    if (!pgd) return false;
    return SetFlasksOnPgd(*pgd, fill);
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
