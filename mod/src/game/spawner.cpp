#include "game/spawner.hpp"
#include "game/memory.hpp"
#include "game/player.hpp"
#include "game/targets.hpp"
#include "net_sync.hpp"
#include "util/config.hpp"
#include "util/log.hpp"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace chaos::spawner {
namespace {

struct Vec3 {
    float x, y, z;
};

struct SpawnDef {
    int model = 0;
    int npc = 0;
    int think = 0;
    int chara = 0;  // CharaInitParam; required for c0000 NPCs (e.g. Gideon)
};

// Hexinton debug spawn request (CHRSP) — WorldChrMan + 0x1E640 -> struct*
constexpr uintptr_t kDbgSpawnPtrOff = 0x1E640;
constexpr uintptr_t kDbgFlagOff = 0x44;
constexpr uintptr_t kDbgPosOff = 0xB0;
constexpr uintptr_t kDbgNpcOff = 0xF0;
constexpr uintptr_t kDbgThinkOff = 0xF4;
constexpr uintptr_t kDbgEventOff = 0xF8;
constexpr uintptr_t kDbgTalkOff = 0xFC;
constexpr uintptr_t kDbgChrIdOff = 0x100;
constexpr uintptr_t kDbgEnemyTypeOff = 0x178;
constexpr uintptr_t kDbgCharaInitOff = 0x17C;
constexpr uintptr_t kDbgManipTypeOff = 0x180;
constexpr size_t kChrSpSize = 0x200;

constexpr uintptr_t kHandleOffset = 0x8;
constexpr uintptr_t kChrIdOffset = 0x188;
constexpr uintptr_t kTeamOffset = 0x6C;
constexpr uintptr_t kModulesOffset = 0x190;
constexpr uintptr_t kPhysicsOffset = 0x68;
constexpr uintptr_t kPosOffset = 0x70;
constexpr uintptr_t kPosAltOffset = 0x80;
constexpr uintptr_t kAng1Offset = 0x54;
constexpr uintptr_t kAng2Offset = 0x64;
constexpr uintptr_t kCharDataOffset = 0x0;
constexpr uintptr_t kHpCur = 0x138;
constexpr uintptr_t kHpMax = 0x144;
constexpr uintptr_t kTimeActOffset = 0x18;
constexpr uintptr_t kAnimQueueOff = 0x20;
constexpr uintptr_t kAnimSlotSize = 16;
constexpr uintptr_t kAnimReadIdxOff = 0xC4;
constexpr uintptr_t kDebugFlagsOff = 0x538;
constexpr uintptr_t kNpcThinkOff = 0x5A0;
constexpr uint32_t kDbgNoMove = 1u << 5;
constexpr int kEnemyTeam = 6;
constexpr float kPosSnapMeters = 0.45f;

// Discovered on current eldenring.exe: Hexinton's 0x1E640 is gone; live CHRSP is 0x1E648.
uintptr_t g_chrsp_world_off = 0x1E648;
bool g_create_disabled = false;

std::string HexU(uintptr_t v) {
    char b[32];
    std::snprintf(b, sizeof(b), "%llX", static_cast<unsigned long long>(v));
    return b;
}

float Dist2(const Vec3& a, const Vec3& b) {
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    const float dz = a.z - b.z;
    return dx * dx + dy * dy + dz * dz;
}

std::optional<uintptr_t> ResolveWorldChrMan(const Config& cfg) {
    auto hit = mem::FindAob(cfg.worldchr_aob);
    if (!hit) return std::nullopt;
    auto slot = mem::ResolveRip(*hit, cfg.worldchr_rip_offset_at, cfg.worldchr_rip_size);
    if (!slot) return std::nullopt;
    uintptr_t world = 0;
    if (!mem::ReadPtr(*slot, world) || !world) return std::nullopt;
    return world;
}

std::optional<uintptr_t> PhysicsOf(uintptr_t chr) {
    uintptr_t modules = 0;
    if (!mem::ReadPtr(chr + kModulesOffset, modules) || !modules) return std::nullopt;
    uintptr_t physics = 0;
    if (!mem::ReadPtr(modules + kPhysicsOffset, physics) || !physics) return std::nullopt;
    return physics;
}

std::optional<Vec3> ReadPosFromChr(uintptr_t chr) {
    auto physics = PhysicsOf(chr);
    if (!physics) return std::nullopt;
    Vec3 pos{};
    if (!mem::Read(*physics + kPosOffset, pos)) return std::nullopt;
    if (!std::isfinite(pos.x) || !std::isfinite(pos.y) || !std::isfinite(pos.z)) return std::nullopt;
    if (pos.x == 0.f && pos.y == 0.f && pos.z == 0.f) return std::nullopt;
    if (std::fabs(pos.x) > 1e6f || std::fabs(pos.y) > 1e6f || std::fabs(pos.z) > 1e6f) return std::nullopt;
    return pos;
}

bool WritePosToChr(uintptr_t chr, const Vec3& pos) {
    auto physics = PhysicsOf(chr);
    if (!physics) return false;
    bool ok = mem::Write(*physics + kPosOffset, pos);
    mem::Write(*physics + kPosAltOffset, pos);
    return ok;
}

bool CharData(uintptr_t chr, uintptr_t& data_out) {
    uintptr_t modules = 0;
    if (!mem::ReadPtr(chr + kModulesOffset, modules) || !modules) return false;
    uintptr_t data = 0;
    if (!mem::ReadPtr(modules + kCharDataOffset, data) || !data) return false;
    data_out = data;
    return true;
}

bool ReadHp(uintptr_t chr, int& hp, int& mx) {
    uintptr_t data = 0;
    if (!CharData(chr, data)) return false;
    if (!mem::Read(data + kHpCur, hp)) return false;
    if (!mem::Read(data + kHpMax, mx) || mx < 1 || mx > 500000) {
        if (!mem::Read(data + 0x13C, mx) || mx < 1 || mx > 500000) return false;
    }
    if (hp < 0 || hp > 500000) return false;
    return true;
}

bool WriteHp(uintptr_t chr, int hp) {
    uintptr_t data = 0;
    if (!CharData(chr, data)) return false;
    if (hp < 0) hp = 0;
    return mem::Write(data + kHpCur, hp);
}

bool ReadHpSeh(uintptr_t chr, int& hp, int& mx) {
    __try {
        return ReadHp(chr, hp, mx);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool WriteHpSeh(uintptr_t chr, int hp) {
    __try {
        return WriteHp(chr, hp);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool WritePosSeh(uintptr_t chr, const Vec3& pos) {
    __try {
        return WritePosToChr(chr, pos);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

struct Facing {
    float a1 = 0;
    float a2 = 0;
};

std::optional<uintptr_t> TimeActOf(uintptr_t chr) {
    uintptr_t modules = 0;
    if (!mem::ReadPtr(chr + kModulesOffset, modules) || !modules) return std::nullopt;
    uintptr_t ta = 0;
    if (!mem::ReadPtr(modules + kTimeActOffset, ta) || !ta) return std::nullopt;
    return ta;
}

uintptr_t AnimSlotAddr(uintptr_t time_act) {
    uint32_t idx = 0;
    if (!mem::Read(time_act + kAnimReadIdxOff, idx) || idx > 9) idx = 0;
    return time_act + kAnimQueueOff + static_cast<uintptr_t>(idx) * kAnimSlotSize;
}

bool ReadFacing(uintptr_t chr, Facing& out) {
    auto physics = PhysicsOf(chr);
    if (!physics) return false;
    if (!mem::Read(*physics + kAng1Offset, out.a1)) return false;
    if (!mem::Read(*physics + kAng2Offset, out.a2)) return false;
    return std::isfinite(out.a1) && std::isfinite(out.a2);
}

bool WriteFacing(uintptr_t chr, float a1, float a2) {
    auto physics = PhysicsOf(chr);
    if (!physics) return false;
    if (!std::isfinite(a1) || !std::isfinite(a2)) return false;
    bool ok = mem::Write(*physics + kAng1Offset, a1);
    ok = mem::Write(*physics + kAng2Offset, a2) && ok;
    return ok;
}

bool ReadAnim(uintptr_t chr, int& id, float& t) {
    auto ta = TimeActOf(chr);
    if (!ta) return false;
    const uintptr_t slot = AnimSlotAddr(*ta);
    if (!mem::Read(slot, id)) return false;
    if (!mem::Read(slot + 4, t)) return false;
    if (id < 0 || id > 20000000) return false;
    if (!std::isfinite(t) || t < 0.f || t > 120.f) t = 0.f;
    return true;
}

bool WriteAnim(uintptr_t chr, int id, float t) {
    if (id < 0 || id > 20000000) return false;
    if (!std::isfinite(t) || t < 0.f) t = 0.f;
    auto ta = TimeActOf(chr);
    if (!ta) return false;
    const uintptr_t slot = AnimSlotAddr(*ta);
    bool ok = mem::Write(slot, id);
    ok = mem::Write(slot + 4, t) && ok;
    ok = mem::Write(slot + 8, t) && ok;
    // Slot 0 is what most tools read; keep it in sync if the ring is elsewhere.
    const uintptr_t slot0 = *ta + kAnimQueueOff;
    if (slot0 != slot) {
        mem::Write(slot0, id);
        mem::Write(slot0 + 4, t);
        mem::Write(slot0 + 8, t);
    }
    return ok;
}

bool FreezePuppet(uintptr_t chr) {
    uint32_t flags = 0;
    if (mem::Read(chr + kDebugFlagsOff, flags)) {
        if ((flags & kDbgNoMove) == 0) {
            flags |= kDbgNoMove;
            mem::Write(chr + kDebugFlagsOff, flags);
        }
    }
    int think = 0;
    if (mem::Read(chr + kNpcThinkOff, think)) {
        if (think != 0 && think > 1000 && think < 2000000000) {
            const int zero = 0;
            mem::Write(chr + kNpcThinkOff, zero);
        }
    }
    return true;
}

bool ReadFacingSeh(uintptr_t chr, Facing& out) {
    __try {
        return ReadFacing(chr, out);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool WriteFacingSeh(uintptr_t chr, float a1, float a2) {
    __try {
        return WriteFacing(chr, a1, a2);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ReadAnimSeh(uintptr_t chr, int& id, float& t) {
    __try {
        return ReadAnim(chr, id, t);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool WriteAnimSeh(uintptr_t chr, int id, float t) {
    __try {
        return WriteAnim(chr, id, t);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool FreezePuppetSeh(uintptr_t chr) {
    __try {
        return FreezePuppet(chr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool EnsureAliveHostile(uintptr_t chr) {
    uintptr_t data = 0;
    if (!CharData(chr, data)) return false;
    int mx = 0;
    if (!mem::Read(data + kHpMax, mx) || mx < 1 || mx > 500000) {
        if (!mem::Read(data + 0x13C, mx) || mx < 1 || mx > 500000) mx = 400;
    }
    if (mx < 50) mx = 400;
    mem::Write(data + 0x13C, mx);
    mem::Write(data + 0x140, mx);
    mem::Write(data + 0x144, mx);
    mem::Write(data + kHpCur, mx);
    mem::Write(chr + kTeamOffset, kEnemyTeam);
    return true;
}

using ApplySpFn = void (*)(void* chr, int id, int unk);

bool InvokeApplySpSeh(uintptr_t fn, uintptr_t chr, int id) {
    __try {
        auto call = reinterpret_cast<ApplySpFn>(fn);
        call(reinterpret_cast<void*>(chr), id, 1);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ApplySpEffect(uintptr_t chr, int id) {
    static uintptr_t fn = 0;
    static bool resolved = false;
    if (!resolved) {
        resolved = true;
        const char* pat = "0F 28 0D ?? ?? ?? ?? ?? 8D ?? ?? 0F 29 ?? ?? ?? 0F B6 D8";
        auto hit = mem::FindUniqueAob(pat);
        if (!hit) hit = mem::FindAob(pat);
        if (hit) {
            fn = *hit - 0x1D;
            log::Info("ApplySpEffect fn=0x" + HexU(fn));
        } else {
            log::Warn("ApplySpEffect AOB not found — HP scale only");
        }
    }
    if (!fn || !chr || id <= 0) return false;
    return InvokeApplySpSeh(fn, chr, id);
}

float BossHealthbars(std::string key) {
    for (char& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (key == "boss_margit" || key == "margit") return 10.f;
    if (key == "boss_godrick" || key == "godrick") return 12.f;
    if (key == "boss_red_wolf" || key == "red_wolf") return 8.f;
    if (key == "boss_rennala" || key == "renala" || key == "rennala") return 11.f;
    if (key == "boss_godfrey_shade" || key == "godfrey_shade") return 13.f;
    if (key == "boss_gideon" || key == "gideon") return 12.f;
    if (key == "boss_godskin" || key == "godskin_duo" || key == "godskin") return 15.f;
    if (key == "boss_maliketh" || key == "maliketh") return 13.f;
    if (key == "boss_fire_giant" || key == "fire_giant") return 18.f;
    if (key == "boss_godfrey" || key == "godfrey" || key == "hoarah") return 16.f;
    if (key == "boss_radagon" || key == "radagon") return 14.f;
    if (key == "boss_elden_beast" || key == "elden_beast") return 18.f;
    return 0.f;
}

int AreaScalingId(int level) {
    if (level < 1) level = 80;
    if (level > 400) level = 400;
    int tier = 1 + ((level - 8) * 20) / 142;
    if (tier < 1) tier = 1;
    if (tier > 21) tier = 21;
    return 7000 + (tier - 1) * 10;
}

void ScaleSpawnedBoss(uintptr_t chr, const std::string& enemy_key) {
    const float bars = BossHealthbars(enemy_key);
    if (bars <= 0.f || !chr) return;
    Config cfg;
    LoadConfig(cfg);
    int level = player::ReadSoulLevel(cfg);
    int php = player::ReadLocalMaxHp(cfg);
    if (level < 1) level = 80;
    if (php < 200) php = 400 + level * 10;
    int target = static_cast<int>(static_cast<float>(php) * bars);
    if (target < 1500) target = 1500;
    if (target > 120000) target = 120000;
    const int sp = AreaScalingId(level);
    if (!ApplySpEffect(chr, sp)) {
        log::Warn("Boss area scaling speffect failed id=" + std::to_string(sp));
    }
    uintptr_t data = 0;
    if (CharData(chr, data)) {
        mem::Write(data + 0x13C, target);
        mem::Write(data + 0x140, target);
        mem::Write(data + 0x144, target);
        mem::Write(data + kHpCur, target);
    }
    log::Info("Boss scale key=" + enemy_key + " RL=" + std::to_string(level) +
              " playerHP=" + std::to_string(php) + " bossHP=" + std::to_string(target) +
              " areaSp=" + std::to_string(sp));
}

bool LooksLikeChrIns(uintptr_t chr) {
    if (!chr || (chr & 0x7) != 0) return false;
    int hp = 0, mx = 0;
    return ReadHp(chr, hp, mx);
}

void CollectFromHandlePool(uintptr_t world, std::unordered_set<uintptr_t>& unique) {
    uintptr_t root = 0;
    if (!mem::ReadPtr(world + 0x1E538, root) || !root) return;
    uintptr_t inner = 0;
    if (!mem::ReadPtr(root + 0x30, inner) || !inner) return;
    uint32_t cnt = 0;
    if (!mem::Read(inner + 0x10, cnt) || cnt == 0 || cnt > 0x2000) return;
    uintptr_t arr = 0;
    if (!mem::ReadPtr(inner + 0x18, arr) || !arr) return;
    for (uint32_t i = 0; i < cnt; ++i) {
        uintptr_t ent = 0;
        if (!mem::ReadPtr(arr + static_cast<uintptr_t>(i) * 16, ent) || !ent) continue;
        if (LooksLikeChrIns(ent)) unique.insert(ent);
    }
}

SpawnDef Def(int model, int npc = -1, int think = -1, int chara = 0) {
    if (npc < 0) npc = model * 10000;
    if (think < 0) think = npc;
    return {model, npc, think, chara};
}

std::vector<SpawnDef> Catalog(std::string key) {
    for (char& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    if (key == "dogs") return {Def(4070), Def(4110), Def(4160)};
    if (key == "rats") return {Def(4080), Def(4090)};
    if (key == "hawks" || key == "warhawks") return {Def(4210)};
    if (key == "lobster" || key == "lobsters") return {Def(4420)};
    if (key == "crabs" || key == "crab") return {Def(2270), Def(2271)};
    // Runebear c4630 (c4980 is Death Rite Bird — that was the wrong spawn)
    if (key == "bear" || key == "bears" || key == "runebear") {
        return {Def(4630, 46300912, 46300000)};
    }
    if (key == "tree_sentinel" || key == "treesentinel") return {Def(3251), Def(3250)};

    // Fog-wall NpcParam rows (not the unscaled *0000 templates). Think IDs stay on the
    // rows we already spawn with so AI does not change.
    if (key == "boss_margit" || key == "margit") return {Def(2130, 21300014, 21300000)};
    if (key == "boss_godrick" || key == "godrick") return {Def(4750, 47500014, 47500000)};
    if (key == "boss_red_wolf" || key == "red_wolf") return {Def(3181, 31810020, 31810000)};
    if (key == "boss_rennala" || key == "renala" || key == "rennala") {
        return {Def(2031, 20310000, 20310000)};
    }
    if (key == "boss_godfrey_shade" || key == "godfrey_shade") {
        return {Def(4720, 47200134, 47200000)};
    }
    if (key == "boss_gideon" || key == "gideon") {
        return {Def(0, 523240070, 523240100, 23241)};
    }
    if (key == "boss_godskin" || key == "godskin_duo") {
        return {Def(3560, 35600172, 35600000), Def(3570, 35700172, 35700000)};
    }
    if (key == "boss_maliketh" || key == "maliketh") return {Def(2110, 21101072, 21100000)};
    if (key == "boss_fire_giant" || key == "fire_giant") return {Def(4760, 47600050, 47600000)};
    if (key == "boss_godfrey" || key == "godfrey" || key == "hoarah") {
        return {Def(4721, 47210070, 47210900)};
    }
    if (key == "boss_radagon" || key == "radagon") return {Def(2190, 21900078, 21900000)};
    if (key == "boss_elden_beast" || key == "elden_beast") return {Def(2200, 22000078, 22000000)};
    return {};
}

std::unordered_set<int> ModelsOf(const std::vector<SpawnDef>& defs) {
    std::unordered_set<int> s;
    for (const auto& d : defs) s.insert(d.model);
    return s;
}

std::string ResolveEnemyKey(std::string key) {
    for (char& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (key == "boss" || key == "boss_random" || key == "random_boss") {
        static const char* bosses[] = {
            "boss_margit", "boss_godrick", "boss_red_wolf", "boss_rennala",
            "boss_godfrey_shade", "boss_gideon", "boss_godskin", "boss_maliketh",
            "boss_fire_giant", "boss_godfrey", "boss_radagon", "boss_elden_beast",
        };
        static bool seeded = false;
        if (!seeded) {
            std::srand(static_cast<unsigned>(std::time(nullptr)) ^ 0xB055u);
            seeded = true;
        }
        return bosses[std::rand() % (sizeof(bosses) / sizeof(bosses[0]))];
    }
    return key;
}

bool AllowRelocateFallback(const std::string& key) {
    // Only dogs/rats/hawks may fall back to relocate. Everything else must CREATE.
    return key == "dogs" || key == "rats" || key == "hawks" || key == "warhawks";
}

bool IdInWanted(int chrId, const std::unordered_set<int>& wanted) {
    if (wanted.count(chrId)) return true;
    if (chrId > 100000 && wanted.count(chrId / 10000)) return true;
    if (chrId > 1000 && wanted.count(chrId / 10)) return true;
    return false;
}

void TryEnableChrSpawnGate() {
    // Only once — forcing this every time was a suspect for reload crashes.
    static bool attempted = false;
    if (attempted) return;
    attempted = true;

    auto hit = mem::FindAob("80 3D ?? ?? ?? ?? 00 0F 28 F0 74 ?? 0F 57 C9");
    if (!hit) hit = mem::FindAob("80 3D ?? ?? ?? ?? 00 0F 85 ?? ?? ?? ?? 32 C0 48");
    if (!hit) return;
    auto addr = mem::ResolveRip(*hit, 2, 7);
    if (!addr) return;
    uint8_t cur = 0;
    if (!mem::Read(*addr, cur)) return;
    if (cur == 0) {
        const uint8_t on = 1;
        if (mem::Write(*addr, on)) log::Info("ChrSpawn gate byte set to 1 (once)");
    }
}

bool WriteChrIdString(uintptr_t spawn, int model) {
    wchar_t buf[8] = {};
    if (std::swprintf(buf, 8, L"c%04d", model) < 0) return false;
    return mem::WriteBytes(spawn + kDbgChrIdOff, buf, 12);
}

bool FillAndTriggerCreateSeh(uintptr_t spawn, int model, int npc, int think, int chara,
                             float x, float y, float z) {
    __try {
        wchar_t buf[8] = {};
        if (std::swprintf(buf, 8, L"c%04d", model) < 0) return false;
        if (!mem::WriteBytes(spawn + kDbgChrIdOff, buf, 12)) return false;
        if (!mem::Write(spawn + kDbgNpcOff, npc)) return false;
        if (!mem::Write(spawn + kDbgThinkOff, think)) return false;
        int zero = 0;
        if (!mem::Write(spawn + kDbgEventOff, zero)) return false;
        if (!mem::Write(spawn + kDbgTalkOff, zero)) return false;
        float pos[3] = {x, y, z};
        if (!mem::WriteBytes(spawn + kDbgPosOff, pos, sizeof(pos))) return false;
        uint8_t enemy_type = 0;
        if (!mem::Write(spawn + kDbgEnemyTypeOff, enemy_type)) return false;
        if (!mem::Write(spawn + kDbgCharaInitOff, chara)) return false;
        if (!mem::Write(spawn + kDbgManipTypeOff, zero)) return false;
        uint8_t flag = 1;
        return mem::Write(spawn + kDbgFlagOff, flag);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool FillAndTriggerCreate(uintptr_t spawn, const SpawnDef& def, const Vec3& pos) {
    return FillAndTriggerCreateSeh(spawn, def.model, def.npc, def.think, def.chara,
                                   pos.x, pos.y, pos.z);
}

bool WaitFlagClear(uintptr_t spawn, int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        uint8_t flag = 1;
        if (mem::Read(spawn + kDbgFlagOff, flag) && flag == 0) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    return false;
}

bool LooksLikeIdleChrSp(uintptr_t spawn) {
    if (!spawn || (spawn & 0x7) != 0) return false;
    uint8_t flag = 0xFF;
    if (!mem::Read(spawn + kDbgFlagOff, flag)) return false;
    if (flag > 1) return false;
    return true;
}

// Read-only: never probe-spawn (that was creating an extra crab + corrupting neighbor fields).
std::optional<uintptr_t> ResolveChrSp(uintptr_t world) {
    TryEnableChrSpawnGate();

    const uintptr_t offs[] = {0x1E648, 0x1E658, 0x1E640};
    for (uintptr_t off : offs) {
        uintptr_t spawn = 0;
        if (!mem::ReadPtr(world + off, spawn)) continue;
        if (!LooksLikeIdleChrSp(spawn)) continue;

        uint8_t flag = 0;
        if (mem::Read(spawn + kDbgFlagOff, flag) && flag != 0) {
            const uint8_t zero = 0;
            mem::Write(spawn + kDbgFlagOff, zero);
        }

        g_chrsp_world_off = off;
        log::Info("CHRSP WorldChrMan+0x" + HexU(off) + " -> 0x" + HexU(spawn));
        return spawn;
    }

    log::Error("CHRSP not ready (null at 0x1E648) — load into a map first");
    return std::nullopt;
}

std::optional<uintptr_t> ResolveOrInstallCreateStruct(uintptr_t world, const Vec3& /*pos*/) {
    return ResolveChrSp(world);
}

int ScoreArrayAt(uintptr_t world, uintptr_t offset, std::vector<uintptr_t>& out_ents) {
    out_ents.clear();
    uintptr_t begin = 0, end = 0;
    if (!mem::ReadPtr(world + offset, begin) || !begin) return 0;
    if (!mem::ReadPtr(world + offset + 8, end) || !end || end <= begin) return 0;
    const size_t bytes = static_cast<size_t>(end - begin);
    if (bytes % sizeof(uintptr_t) != 0) return 0;
    const size_t count = bytes / sizeof(uintptr_t);
    if (count < 2 || count > 2048) return 0;
    int valid = 0;
    for (size_t i = 0; i < count; ++i) {
        uintptr_t ent = 0;
        if (!mem::ReadPtr(begin + i * sizeof(uintptr_t), ent) || !ent) continue;
        if (!LooksLikeChrIns(ent)) continue;
        out_ents.push_back(ent);
        ++valid;
    }
    return valid;
}

std::vector<uintptr_t> CollectEntities(uintptr_t world) {
    const uintptr_t known[] = {
        0x1F1B8, 0x1F1B0, 0x1F1D0, 0x1E6B8, 0x1E6C0, 0x1F190,
        0x1F170, 0x1EF50, 0x1F0B0, 0x1E508, 0x1DC50
    };
    std::unordered_set<uintptr_t> unique;
    for (uintptr_t off : known) {
        std::vector<uintptr_t> ents;
        if (ScoreArrayAt(world, off, ents) > 0) {
            for (auto e : ents) unique.insert(e);
        }
    }
    CollectFromHandlePool(world, unique);
    if (unique.size() < 8) {
        for (uintptr_t off = 0x18000; off < 0x22000; off += 0x8) {
            std::vector<uintptr_t> ents;
            if (ScoreArrayAt(world, off, ents) > 0) {
                for (auto e : ents) unique.insert(e);
            }
            if (unique.size() >= 64) break;
        }
    }
    return std::vector<uintptr_t>(unique.begin(), unique.end());
}

void StickAlive(std::vector<std::pair<uintptr_t, Vec3>> targets) {
    for (int i = 0; i < 120; ++i) {
        for (const auto& [chr, pos] : targets) {
            WritePosToChr(chr, pos);
            EnsureAliveHostile(chr);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
}

uintptr_t CreateOneAt(uintptr_t world, const SpawnDef& def, const Vec3& pos,
                      const std::string& enemy_key);

void RegisterBossDrop(uintptr_t chr, const std::string& enemy_key);

int TryCreateNear(uintptr_t world, uintptr_t /*spawn_hint*/, const Vec3& at,
                  const std::vector<SpawnDef>& defs, int count, const std::string& enemy_key) {
    if (g_create_disabled || defs.empty()) return 0;

    int created = 0;
    for (int i = 0; i < count; ++i) {
        const SpawnDef& def = defs[static_cast<size_t>(i) % defs.size()];
        const float angle = (6.2831853f / static_cast<float>(count)) * static_cast<float>(i);
        Vec3 pos{at.x + std::cos(angle) * 4.0f, at.y + 0.5f, at.z + std::sin(angle) * 4.0f};
        if (CreateOneAt(world, def, pos, enemy_key)) ++created;
    }
    return created;
}

uintptr_t FindNewChr(uintptr_t world, const std::unordered_set<uintptr_t>& before,
                     const std::unordered_set<int>& wanted, const Vec3& at) {
    uintptr_t best_match = 0, best_any = 0;
    float match_d = 1e30f, any_d = 1e30f;
    for (uintptr_t ent : CollectEntities(world)) {
        if (before.count(ent)) continue;
        auto pos = ReadPosFromChr(ent);
        const float d = pos ? Dist2(*pos, at) : 0.f;
        int id = 0;
        mem::Read(ent + kChrIdOffset, id);
        if (IdInWanted(id, wanted) && d < match_d) {
            match_d = d;
            best_match = ent;
        }
        if (d < any_d) {
            any_d = d;
            best_any = ent;
        }
    }
    return best_match ? best_match : best_any;
}

struct WatchedBoss {
    uintptr_t chr = 0;
    std::string key;
    bool seen_alive = false;
    int miss = 0;
};

std::mutex g_drops_mu;
std::vector<WatchedBoss> g_drops;
std::atomic<bool> g_drop_thread{false};

void DropThread() {
    while (true) {
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        std::vector<std::string> dead;
        {
            std::lock_guard lock(g_drops_mu);
            for (auto it = g_drops.begin(); it != g_drops.end();) {
                int hp = 0, mx = 0;
                if (!ReadHpSeh(it->chr, hp, mx)) {
                    ++it->miss;
                    const int limit = it->seen_alive ? 40 : 80;
                    if (it->miss >= limit) {
                        it = g_drops.erase(it);
                    } else {
                        ++it;
                    }
                    continue;
                }
                it->miss = 0;
                if (hp > 0) {
                    it->seen_alive = true;
                    ++it;
                    continue;
                }
                if (it->seen_alive && hp <= 0) {
                    dead.push_back(it->key);
                    it = g_drops.erase(it);
                    continue;
                }
                ++it;
            }
        }
        if (dead.empty()) continue;

        Config cfg;
        LoadConfig(cfg);
        const int before = player::ReadRunes(cfg);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        const int after = player::ReadRunes(cfg);
        int native = 0;
        if (before >= 0 && after >= before) native = after - before;

        int need = 0;
        for (const auto& key : dead) {
            const int runes = player::GiveBossKillItems(key);
            if (runes > 0) need += runes;
        }
        if (need > native) {
            player::AddRunes(cfg, need - native);
        }
    }
}

void RegisterBossDrop(uintptr_t chr, const std::string& enemy_key) {
    if (!chr || !player::HasBossDrops(enemy_key)) return;
    {
        std::lock_guard lock(g_drops_mu);
        if (g_drops.size() >= 24) {
            g_drops.erase(g_drops.begin());
        }
        g_drops.push_back({chr, enemy_key, false, 0});
    }
    log::Info("Boss drop watch key=" + enemy_key + " chr=0x" + HexU(chr));
    if (g_drop_thread.exchange(true)) return;
    std::thread(DropThread).detach();
}

uintptr_t CreateOneAt(uintptr_t world, const SpawnDef& def, const Vec3& pos,
                      const std::string& enemy_key) {
    std::unordered_set<uintptr_t> before;
    for (uintptr_t e : CollectEntities(world)) before.insert(e);

    auto spawn_opt = ResolveChrSp(world);
    if (!spawn_opt) {
        log::Warn("CREATE abort: CHRSP gone");
        return 0;
    }
    uintptr_t spawn = *spawn_opt;
    if (!FillAndTriggerCreate(spawn, def, pos)) {
        uint8_t flag = 0xFF;
        mem::Read(spawn + kDbgFlagOff, flag);
        if (flag != 1) {
            log::Warn("Create fill failed for c" + std::to_string(def.model));
            return 0;
        }
    }
    const bool cleared = WaitFlagClear(spawn, 1500);
    if (!cleared) {
        const uint8_t zero = 0;
        mem::Write(spawn + kDbgFlagOff, zero);
        log::Warn("CREATE flag stuck for c" + std::to_string(def.model) + " — forced idle");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(cleared ? 250 : 500));

    const auto wanted = ModelsOf({def});
    uintptr_t chr = FindNewChr(world, before, wanted, pos);
    if (!chr) {
        log::Warn("CREATE no new chr for c" + std::to_string(def.model));
        return 0;
    }
    log::Info("CREATE ok c" + std::to_string(def.model) + " npc=" + std::to_string(def.npc) +
              " chr=0x" + HexU(chr) + " flagCleared=" + (cleared ? "1" : "0"));
    EnsureAliveHostile(chr);
    ScaleSpawnedBoss(chr, enemy_key);
    RegisterBossDrop(chr, enemy_key);
    return chr;
}

int TryRelocateNear(uintptr_t world, uintptr_t avoid_chr, const Vec3& at, float radius,
                    const std::unordered_set<int>& wanted, int count, const std::string& enemy_key) {
    auto entities = CollectEntities(world);
    struct Candidate {
        uintptr_t chr;
        float dist2;
        bool alive;
        int id;
    };
    std::vector<Candidate> matched;
    for (uintptr_t ent : entities) {
        if (ent == avoid_chr) continue;
        auto pos = ReadPosFromChr(ent);
        if (!pos) continue;
        int id = 0;
        if (!mem::Read(ent + kChrIdOffset, id)) continue;
        if (!IdInWanted(id, wanted)) continue;
        int hp = 0, mx = 0;
        if (!ReadHp(ent, hp, mx)) continue;
        matched.push_back({ent, Dist2(*pos, at), hp > 0, id});
    }
    std::sort(matched.begin(), matched.end(), [](const Candidate& a, const Candidate& b) {
        if (a.alive != b.alive) return a.alive > b.alive;
        return a.dist2 < b.dist2;
    });

    log::Info("Relocate matched " + std::to_string(matched.size()));
    if (matched.empty()) {
        std::string sample = "No '" + enemy_key + "' loaded. Nearby chrIds:";
        int n = 0;
        for (uintptr_t ent : entities) {
            if (ent == avoid_chr) continue;
            auto pos = ReadPosFromChr(ent);
            if (!pos || Dist2(*pos, at) > 400.f * 400.f) continue;
            int id = 0;
            if (!mem::Read(ent + kChrIdOffset, id)) continue;
            sample += " " + std::to_string(id);
            if (++n >= 40) break;
        }
        log::Warn(sample);
        return 0;
    }

    std::vector<std::pair<uintptr_t, Vec3>> sticky;
    std::unordered_set<uintptr_t> used;
    for (const auto& c : matched) {
        if (static_cast<int>(sticky.size()) >= count) break;
        if (used.count(c.chr)) continue;
        used.insert(c.chr);
        const int i = static_cast<int>(sticky.size());
        const float angle = (6.2831853f / static_cast<float>(std::max(count, 1))) * static_cast<float>(i);
        Vec3 pos{at.x + std::cos(angle) * radius, at.y + 0.5f, at.z + std::sin(angle) * radius};
        EnsureAliveHostile(c.chr);
        WritePosToChr(c.chr, pos);
        sticky.emplace_back(c.chr, pos);
        log::Info("Relocate #" + std::to_string(i + 1) + " id=" + std::to_string(c.id) +
                  (c.alive ? " alive" : " revived"));
    }
    if (sticky.empty()) return 0;
    const int moved = static_cast<int>(sticky.size());
    std::thread([sticky = std::move(sticky)]() mutable { StickAlive(std::move(sticky)); }).detach();
    return moved;
}

struct SyncedEnt {
    uint64_t handle = 0;
    uintptr_t chr = 0;
    int last_applied_hp = -1;
    int last_dmg_sent = -1;
    int last_anim = -1;
    uint32_t last_seq = 0;
    bool dead = false;
    int dead_echo = 0;
    bool puppet_ready = false;
};

std::mutex g_ents_mu;
std::vector<SyncedEnt> g_ents;
std::atomic<uint32_t> g_seq{1};
std::atomic<bool> g_sync_started{false};

void EnsureChr(SyncedEnt& e) {
    int hp = 0, mx = 0;
    if (e.chr && ReadHpSeh(e.chr, hp, mx)) return;
    e.chr = 0;
}

void TickSyncedOnce() {
    struct Packet {
        uint64_t handle = 0;
        int hp = 0;
        int mx = 0;
        float x = 0, y = 0, z = 0;
        float ang1 = 0, ang2 = 0;
        int anim = 0;
        float anim_t = 0;
        bool dead = false;
        bool have_pos = false;
        bool have_ang = false;
        bool have_anim = false;
    };
    std::vector<Packet> outbox;
    std::vector<std::pair<uint64_t, int>> dmgbox;

    {
        std::lock_guard lock(g_ents_mu);
        if (g_ents.empty()) return;

        const bool client = net::IsClient();
        for (auto it = g_ents.begin(); it != g_ents.end();) {
            SyncedEnt& e = *it;
            EnsureChr(e);
            int hp = 0, mx = 0;
            const bool alive_read = e.chr && ReadHpSeh(e.chr, hp, mx);
            if (!alive_read) {
                hp = 0;
                e.dead = true;
            }
            if (hp <= 0) e.dead = true;

            if (client) {
                if (e.chr && !e.dead) FreezePuppetSeh(e.chr);
                if (!e.dead && e.last_applied_hp > 0 && hp < e.last_applied_hp && hp != e.last_dmg_sent) {
                    dmgbox.emplace_back(e.handle, hp);
                    e.last_dmg_sent = hp;
                }
            } else if (net::ClientCount() > 0 && e.chr) {
                Packet p;
                p.handle = e.handle;
                p.hp = hp;
                p.mx = mx;
                p.dead = e.dead;
                if (auto pos = ReadPosFromChr(e.chr)) {
                    p.x = pos->x;
                    p.y = pos->y;
                    p.z = pos->z;
                    p.have_pos = true;
                }
                Facing fac{};
                if (ReadFacingSeh(e.chr, fac)) {
                    p.ang1 = fac.a1;
                    p.ang2 = fac.a2;
                    p.have_ang = true;
                }
                int anim = 0;
                float at = 0.f;
                if (ReadAnimSeh(e.chr, anim, at)) {
                    p.anim = anim;
                    p.anim_t = at;
                    p.have_anim = true;
                }
                if (p.have_pos) outbox.push_back(p);
            }

            if (e.dead) {
                ++e.dead_echo;
                if (e.dead_echo >= 8) {
                    it = g_ents.erase(it);
                    continue;
                }
            } else {
                e.dead_echo = 0;
            }
            ++it;
        }
    }

    for (const auto& [h, hp] : dmgbox) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "{\"cmd\":\"dmg\",\"h\":\"%llX\",\"hp\":%d}",
                      static_cast<unsigned long long>(h), hp);
        net::SendToHost(buf);
    }
    for (const auto& p : outbox) {
        const uint32_t n = g_seq.fetch_add(1);
        char buf[384];
        if (p.have_anim && p.have_ang) {
            std::snprintf(buf, sizeof(buf),
                          "{\"cmd\":\"ent\",\"n\":%u,\"h\":\"%llX\",\"hp\":%d,\"m\":%d,"
                          "\"x\":%.2f,\"y\":%.2f,\"z\":%.2f,\"d\":%d,"
                          "\"a\":%d,\"t\":%.3f,\"r\":%.4f,\"q\":%.4f}",
                          n, static_cast<unsigned long long>(p.handle), p.hp, p.mx,
                          p.x, p.y, p.z, p.dead ? 1 : 0,
                          p.anim, p.anim_t, p.ang1, p.ang2);
        } else {
            std::snprintf(buf, sizeof(buf),
                          "{\"cmd\":\"ent\",\"n\":%u,\"h\":\"%llX\",\"hp\":%d,\"m\":%d,"
                          "\"x\":%.2f,\"y\":%.2f,\"z\":%.2f,\"d\":%d}",
                          n, static_cast<unsigned long long>(p.handle), p.hp, p.mx,
                          p.x, p.y, p.z, p.dead ? 1 : 0);
        }
        net::Broadcast(buf);
    }
}

void SyncThread() {
    while (true) {
        TickSyncedOnce();
        std::this_thread::sleep_for(std::chrono::milliseconds(33));
    }
}

void EnsureSyncThread() {
    if (g_sync_started.exchange(true)) return;
    std::thread(SyncThread).detach();
}

}  // namespace

bool SpawnNearTargets(const Config& cfg, const std::string& enemy_key_in, int count, targets::Mode mode) {
    if (!cfg.spawn_enabled) {
        log::Warn("Spawn disabled");
        return false;
    }
    if (count < 1) count = 1;
    if (count > 12) count = 12;

    const std::string enemy_key = ResolveEnemyKey(enemy_key_in);
    const auto defs = Catalog(enemy_key);
    if (defs.empty()) {
        log::Error("Unknown enemy key: " + enemy_key_in);
        return false;
    }
    const auto wanted = ModelsOf(defs);

    auto world = ResolveWorldChrMan(cfg);
    if (!world) return false;

    auto dests = targets::SelectTargets(cfg, mode);
    if (dests.empty()) {
        log::Error("Spawn: no player targets");
        return false;
    }

    const float radius = std::max(cfg.spawn_radius, 3.5f);
    int total = 0;

    for (const auto& dest : dests) {
        // WorldChrMan pointer can change after death / site-of-grace reload.
        world = ResolveWorldChrMan(cfg);
        if (!world) {
            log::Error("Spawn: WorldChrMan lost mid-batch");
            break;
        }

        auto pos = ReadPosFromChr(dest.chr);
        if (!pos) {
            log::Warn("Spawn: target pos unread");
            continue;
        }
        log::Info("Spawn " + enemy_key + " x" + std::to_string(count) + " near " +
                  (dest.is_local ? "host" : ("slot" + std::to_string(dest.slot))) +
                  " model=c" + std::to_string(defs[0].model));

        int n = 0;
        if (auto spawn = ResolveOrInstallCreateStruct(*world, *pos)) {
            n = TryCreateNear(*world, *spawn, *pos, defs, count, enemy_key);
        } else {
            log::Error("CHRSP unavailable on this game build");
        }

        if (n < count && AllowRelocateFallback(enemy_key)) {
            log::Warn("Create got " + std::to_string(n) + "/" + std::to_string(count) +
                      " — relocate fallback for " + enemy_key);
            n += TryRelocateNear(*world, dest.chr, *pos, radius, wanted, count - n, enemy_key);
        } else if (n <= 0) {
            log::Error("CREATE failed for " + enemy_key +
                       " and relocate fallback disabled (won't teleport existing)");
        }
        total += n;
    }

    if (total <= 0) {
        log::Error("Spawn failed for " + enemy_key);
        return false;
    }
    log::Info("Spawn ok total=" + std::to_string(total) + " key=" + enemy_key);
    return true;
}

bool SpawnNearPlayer(const Config& cfg, const std::string& enemy_key, int count) {
    return SpawnNearTargets(cfg, enemy_key, count, targets::Mode::All);
}

std::string ResolveKey(const std::string& enemy_key) {
    return ResolveEnemyKey(enemy_key);
}

bool PickParticipantPosition(const Config& cfg, int participant, float& x, float& y, float& z) {
    auto players = targets::CollectPlayers(cfg);
    if (players.empty()) return false;

    uintptr_t chr = 0;
    if (participant <= 0) {
        for (const auto& p : players) {
            if (p.is_local) { chr = p.chr; break; }
        }
        if (!chr) chr = players[0].chr;
    } else {
        int seen = 0;
        for (const auto& p : players) {
            if (p.is_local) continue;
            if (++seen == participant) { chr = p.chr; break; }
        }
    }
    if (!chr) return false;
    auto pos = ReadPosFromChr(chr);
    if (!pos) return false;
    x = pos->x;
    y = pos->y;
    z = pos->z;
    return true;
}

bool SpawnAtPosition(const Config& cfg, const std::string& enemy_key_in, int count,
                     float x, float y, float z) {
    if (!cfg.spawn_enabled) return false;
    if (count < 1) count = 1;
    if (count > 12) count = 12;

    const std::string enemy_key = ResolveEnemyKey(enemy_key_in);
    const auto defs = Catalog(enemy_key);
    if (defs.empty()) {
        log::Error("Unknown enemy key: " + enemy_key_in);
        return false;
    }

    auto world = ResolveWorldChrMan(cfg);
    if (!world) return false;

    const Vec3 pos{x, y, z};
    log::Info("Synced spawn " + enemy_key + " x" + std::to_string(count) + " at fixed position");

    int n = 0;
    if (auto spawn = ResolveOrInstallCreateStruct(*world, pos)) {
        n = TryCreateNear(*world, *spawn, pos, defs, count, enemy_key);
    }
    if (n <= 0) {
        log::Error("Synced spawn failed for " + enemy_key);
        return false;
    }
    log::Info("Synced spawn ok total=" + std::to_string(n) + " key=" + enemy_key);
    return true;
}

bool ReserveRuntimeHandles(const Config& cfg, int count, std::vector<uint64_t>& out_handles) {
    (void)cfg;
    out_handles.clear();
    if (count < 1) count = 1;
    if (count > 12) count = 12;
    static std::atomic<uint32_t> next{1};
    const uint32_t base = next.fetch_add(static_cast<uint32_t>(count));
    for (int i = 0; i < count; ++i) {
        const uint64_t h = 0xC4A0500000000000ull | (static_cast<uint64_t>(base) + static_cast<uint64_t>(i));
        out_handles.push_back(h);
        log::Info("Sync id=" + HexU(static_cast<uintptr_t>(h)));
    }
    return true;
}

bool SpawnCreatedWithHandles(const Config& cfg, const std::string& enemy_key_in,
                             const std::vector<uint64_t>& handles, float x, float y, float z) {
    if (!cfg.spawn_enabled || handles.empty()) return false;

    const std::string enemy_key = ResolveEnemyKey(enemy_key_in);
    const auto defs = Catalog(enemy_key);
    if (defs.empty()) {
        log::Error("Unknown enemy key: " + enemy_key_in);
        return false;
    }

    auto world = ResolveWorldChrMan(cfg);
    if (!world) return false;

    const Vec3 at{x, y, z};
    log::Info("Coop create " + enemy_key + " x" + std::to_string(handles.size()) +
              (net::IsClient() ? " (client)" : " (host)"));

    int ok = 0;
    for (size_t i = 0; i < handles.size(); ++i) {
        const SpawnDef& def = defs[i % defs.size()];
        const float angle = (6.2831853f / static_cast<float>(handles.size())) * static_cast<float>(i);
        Vec3 pos{at.x + std::cos(angle) * 4.0f, at.y + 0.5f, at.z + std::sin(angle) * 4.0f};
        uintptr_t chr = CreateOneAt(*world, def, pos, enemy_key);
        if (!chr) continue;
        WritePosToChr(chr, pos);
        if (net::IsClient()) FreezePuppetSeh(chr);
        RegisterSynced(handles[i], chr);
        ++ok;
    }
    if (ok <= 0) {
        log::Error("Coop create failed for " + enemy_key);
        return false;
    }
    log::Info("Coop create ok " + std::to_string(ok) + "/" + std::to_string(handles.size()) +
              " key=" + enemy_key);
    return true;
}

void RegisterSynced(uint64_t handle, uintptr_t chr) {
    if (!handle) return;
    std::lock_guard lock(g_ents_mu);
    for (auto& e : g_ents) {
        if (e.handle == handle) {
            e.chr = chr;
            e.dead = false;
            e.dead_echo = 0;
            e.last_anim = -1;
            e.puppet_ready = false;
            EnsureSyncThread();
            return;
        }
    }
    SyncedEnt e;
    e.handle = handle;
    e.chr = chr;
    g_ents.push_back(e);
    EnsureSyncThread();
    log::Info("Entity sync tracking handle=" + HexU(static_cast<uintptr_t>(handle)));
}

void ApplyEntState(uint64_t handle, uint32_t seq, int hp, int mx, float x, float y, float z, bool dead,
                   int anim_id, float anim_t, bool have_rot, float ang1, float ang2) {
    (void)mx;
    if (!net::IsClient() || !handle) return;
    std::lock_guard lock(g_ents_mu);
    SyncedEnt* e = nullptr;
    for (auto& it : g_ents) {
        if (it.handle == handle) {
            e = &it;
            break;
        }
    }
    if (!e) {
        g_ents.push_back({});
        e = &g_ents.back();
        e->handle = handle;
        EnsureSyncThread();
    }
    if (seq != 0 && seq <= e->last_seq) return;
    if (seq != 0) e->last_seq = seq;
    EnsureChr(*e);
    if (!e->chr) return;

    FreezePuppetSeh(e->chr);
    if (!e->puppet_ready) {
        e->puppet_ready = true;
        log::Info("Puppet: local AI frozen, copying host anim");
    }

    const bool have_pos = std::isfinite(x) && std::isfinite(y) && std::isfinite(z) &&
                          !(x == 0.f && y == 0.f && z == 0.f);
    bool anim_changed = false;
    if (anim_id >= 0) {
        if (e->last_anim != anim_id) {
            anim_changed = true;
            e->last_anim = anim_id;
        }
    }

    // Hard snaps cancel attacks. Only correct when the copy drifted, or a new move started.
    if (have_pos) {
        float d2 = kPosSnapMeters * kPosSnapMeters + 1.f;
        if (auto cur = ReadPosFromChr(e->chr)) {
            d2 = Dist2(*cur, Vec3{x, y, z});
        }
        if (anim_changed || d2 > kPosSnapMeters * kPosSnapMeters) {
            WritePosSeh(e->chr, Vec3{x, y, z});
        }
    }
    if (have_rot) WriteFacingSeh(e->chr, ang1, ang2);
    if (anim_id >= 0) {
        const float t = (std::isfinite(anim_t) && anim_t >= 0.f) ? anim_t : 0.f;
        int local_id = -1;
        float local_t = 0.f;
        const bool got = ReadAnimSeh(e->chr, local_id, local_t);
        // Rewriting the same anim id every tick restarts the clip and looks like a cancel.
        if (!got || local_id != anim_id) {
            WriteAnimSeh(e->chr, anim_id, t);
        } else if (std::fabs(local_t - t) > 0.12f) {
            WriteAnimSeh(e->chr, anim_id, t);
        }
    }

    if (dead || hp <= 0) {
        WriteHpSeh(e->chr, 0);
        e->dead = true;
        e->last_applied_hp = 0;
        return;
    }
    int local = 0, local_mx = 0;
    if (!ReadHpSeh(e->chr, local, local_mx)) local = hp;
    if (e->last_applied_hp < 0 || hp <= local) {
        WriteHpSeh(e->chr, hp);
        e->last_applied_hp = hp;
        e->last_dmg_sent = hp;
    }
}

void ApplyEntDamage(uint64_t handle, int hp) {
    if (net::IsClient() || !handle) return;
    std::lock_guard lock(g_ents_mu);
    for (auto& e : g_ents) {
        if (e.handle != handle) continue;
        EnsureChr(e);
        if (!e.chr) return;
        int cur = 0, mx = 0;
        if (!ReadHpSeh(e.chr, cur, mx)) return;
        if (hp >= 0 && hp < cur) {
            WriteHpSeh(e.chr, hp);
            if (hp <= 0) e.dead = true;
        }
        return;
    }
}

}  // namespace chaos::spawner
