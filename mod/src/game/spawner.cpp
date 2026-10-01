#include "game/spawner.hpp"
#include "game/memory.hpp"
#include "game/params.hpp"
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
#include <random>
#include <string>
#include <unordered_map>
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

constexpr uintptr_t kChrIdOffset = 0x188;
constexpr uintptr_t kTeamOffset = 0x6C;
constexpr uintptr_t kModulesOffset = 0x190;
constexpr uintptr_t kPhysicsOffset = 0x68;
constexpr uintptr_t kPosOffset = 0x70;
constexpr uintptr_t kPosAltOffset = 0x80;
constexpr uintptr_t kProxyPosRequestOff = 0x91;
constexpr uintptr_t kChrCtrlOffset = 0x58;
constexpr uintptr_t kChrProxyFlagsOff = 0xFC;
constexpr uint32_t kProxySyncPos = 1u << 0;
constexpr uintptr_t kCharDataOffset = 0x0;
constexpr uintptr_t kSuperArmorOffset = 0x40;
constexpr uintptr_t kSaCur = 0x10;
constexpr uintptr_t kSaMax = 0x14;
constexpr uintptr_t kHpCur = 0x138;
constexpr uintptr_t kHpMax = 0x144;
constexpr uintptr_t kLocalPlayerOff = 0x1E508;
constexpr int kEnemyTeam = 6;

// NpcParam / ItemLotParam field offsets (Paramdex ER defs).
constexpr uintptr_t kNpcGetSoul = 0x2C;
constexpr uintptr_t kNpcItemLotEnemy = 0x30;
constexpr uintptr_t kLotIds = 0x00;
constexpr uintptr_t kLotCategories = 0x20;
constexpr uintptr_t kLotPoints = 0x40;
constexpr uintptr_t kLotCounts = 0x8A;

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
    // The pattern slot is stable. Scanning the exe on the sync thread was
    // stalling the game and flooding the log.
    static uintptr_t slot = 0;
    if (!slot) {
        auto hit = mem::FindAob(cfg.worldchr_aob);
        if (!hit) return std::nullopt;
        auto resolved = mem::ResolveRip(*hit, cfg.worldchr_rip_offset_at, cfg.worldchr_rip_size);
        if (!resolved) return std::nullopt;
        slot = *resolved;
    }
    uintptr_t world = 0;
    if (!mem::ReadPtr(slot, world) || !world) return std::nullopt;
    return world;
}

const Config& SharedConfig() {
    static Config cfg;
    static bool loaded = false;
    if (!loaded) {
        LoadConfig(cfg);
        loaded = true;
    }
    return cfg;
}

bool InImage(uintptr_t a) {
    const uintptr_t b = mem::ModuleBase();
    const size_t n = mem::ModuleSize();
    return b && n && a >= b && a < b + n;
}

bool RttiIs(uintptr_t obj, const char* want) {
    uintptr_t vt = 0;
    if (!mem::ReadPtr(obj, vt) || !InImage(vt)) return false;
    uintptr_t col = 0;
    if (!mem::ReadPtr(vt - 8, col) || !InImage(col)) return false;
    uint32_t sig = 0;
    if (!mem::Read(col, sig) || sig != 1) return false;
    uint32_t td = 0;
    if (!mem::Read(col + 0x0C, td) || td == 0) return false;
    char buf[96]{};
    if (!mem::ReadBytes(mem::ModuleBase() + td + 0x10, buf, sizeof(buf) - 1)) return false;
    buf[sizeof(buf) - 1] = '\0';
    return std::strcmp(buf, want) == 0;
}

std::optional<uintptr_t> ModuleOf(uintptr_t chr, uintptr_t slot) {
    uintptr_t modules = 0;
    if (!mem::ReadPtr(chr + kModulesOffset, modules) || !modules) return std::nullopt;
    uintptr_t mod = 0;
    if (!mem::ReadPtr(modules + slot, mod) || !mod) return std::nullopt;
    return mod;
}

// The Havok capsule keeps its own copy of the transform. Moving only the physics
// module fights the capsule every frame: bodies sink, float, or get launched.
void RequestProxySync(uintptr_t chr, uintptr_t physics) {
    uintptr_t ctrl = 0;
    if (mem::ReadPtr(chr + kChrCtrlOffset, ctrl) && ctrl && RttiIs(ctrl, ".?AVChrCtrl@CS@@")) {
        uint32_t flags = 0;
        if (mem::Read(ctrl + kChrProxyFlagsOff, flags)) mem::Write(ctrl + kChrProxyFlagsOff, flags | kProxySyncPos);
    }
    const uint8_t one = 1;
    mem::Write(physics + kProxyPosRequestOff, one);
}

std::optional<Vec3> ReadPosFromChr(uintptr_t chr) {
    auto physics = ModuleOf(chr, kPhysicsOffset);
    if (!physics) return std::nullopt;
    Vec3 pos{};
    if (!mem::Read(*physics + kPosOffset, pos)) return std::nullopt;
    if (!std::isfinite(pos.x) || !std::isfinite(pos.y) || !std::isfinite(pos.z)) return std::nullopt;
    if (pos.x == 0.f && pos.y == 0.f && pos.z == 0.f) return std::nullopt;
    if (std::fabs(pos.x) > 1e6f || std::fabs(pos.y) > 1e6f || std::fabs(pos.z) > 1e6f) return std::nullopt;
    return pos;
}

// Only used by the dogs/rats/hawks relocate fallback, never on a chaos spawn.
bool WritePosToChr(uintptr_t chr, const Vec3& pos) {
    auto physics = ModuleOf(chr, kPhysicsOffset);
    if (!physics) return false;
    bool ok = mem::Write(*physics + kPosOffset, pos);
    mem::Write(*physics + kPosAltOffset, pos);
    if (ok) RequestProxySync(chr, *physics);
    return ok;
}

bool CharData(uintptr_t chr, uintptr_t& data_out) {
    auto data = ModuleOf(chr, kCharDataOffset);
    if (!data) return false;
    data_out = *data;
    return true;
}

int HighestMaxHp(uintptr_t data);

bool ReadHp(uintptr_t chr, int& hp, int& mx) {
    uintptr_t data = 0;
    if (!CharData(chr, data)) return false;
    if (!mem::Read(data + kHpCur, hp)) return false;
    mx = HighestMaxHp(data);
    if (mx < 1 || hp < 0 || hp > 500000) return false;
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

// Stance (posture) of an enemy. It drains on hits and breaks into a stagger at 0.
bool ReadPosture(uintptr_t chr, uintptr_t& sa_mod, float& cur, float& mx) {
    auto mod = ModuleOf(chr, kSuperArmorOffset);
    if (!mod) return false;
    if (!mem::Read(*mod + kSaCur, cur) || !mem::Read(*mod + kSaMax, mx)) return false;
    if (!std::isfinite(cur) || !std::isfinite(mx) || mx <= 0.f || mx > 100000.f) return false;
    if (cur < -1.f || cur > mx + 1.f) return false;
    sa_mod = *mod;
    return true;
}

bool ReadPostureSeh(uintptr_t chr, uintptr_t& sa_mod, float& cur, float& mx) {
    __try {
        return ReadPosture(chr, sa_mod, cur, mx);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

int HighestMaxHp(uintptr_t data) {
    int best = 0;
    const uintptr_t offs[] = {0x13C, 0x140, 0x144};
    for (uintptr_t off : offs) {
        int v = 0;
        if (mem::Read(data + off, v) && v > best && v <= 500000) best = v;
    }
    return best;
}

int ForceFullHp(uintptr_t chr) {
    // Leave the param max alone. Writing it too early used to freeze a partial
    // value, so the body spawned already hurt. Only raise current HP up to the
    // highest max the game has published.
    uintptr_t data = 0;
    if (!CharData(chr, data)) return 0;
    const int mx = HighestMaxHp(data);
    if (mx < 1) return 0;
    mem::Write(data + kHpCur, mx);
    mem::Write(chr + kTeamOffset, kEnemyTeam);
    return mx;
}

int ForceFullHpSeh(uintptr_t chr) {
    __try {
        return ForceFullHp(chr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

bool EnsureAliveHostile(uintptr_t chr) {
    return ForceFullHp(chr) > 0;
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

    // Npc row = the fight you meet them in (that row carries HP and damage).
    // Think = the combat AI. The 900-phase thinks wait on a boss-room event,
    // which left Godfrey invisible and pulled players into an unloaded void.
    if (key == "boss_margit" || key == "margit") return {Def(2130, 21300014, 21300000)};
    if (key == "boss_godrick" || key == "godrick") return {Def(4750, 47500014, 47500000)};
    if (key == "boss_red_wolf" || key == "red_wolf") return {Def(3181, 31811024, 31810000)};
    if (key == "boss_rennala" || key == "renala" || key == "rennala") return {Def(2031, 20310024, 20310000)};
    if (key == "boss_godfrey_shade" || key == "godfrey_shade") {
        return {Def(4720, 47200134, 47200000)};
    }
    if (key == "boss_gideon" || key == "gideon") {
        return {Def(0, 523240070, 523240100, 23241)};
    }
    if (key == "boss_godskin" || key == "godskin_duo") {
        return {Def(3560, 35600172, 35600000), Def(3570, 35700172, 35700000)};
    }
    if (key == "boss_maliketh" || key == "maliketh") return {Def(2110, 21100072, 21100000)};
    if (key == "boss_malenia" || key == "malenia") return {Def(2120, 21200056, 21200000)};
    if (key == "boss_fire_giant" || key == "fire_giant") return {Def(4760, 47600050, 47600000)};
    if (key == "boss_godfrey" || key == "godfrey" || key == "hoarah") {
        return {Def(4720, 47200070, 47200000)};
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
            "boss_malenia", "boss_fire_giant", "boss_godfrey", "boss_radagon", "boss_elden_beast",
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
    for (int i = 0; i < 180; ++i) {
        for (const auto& [chr, pos] : targets) {
            WritePosToChr(chr, pos);
            EnsureAliveHostile(chr);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
}

bool IsLocalPlayer(uintptr_t chr) {
    if (!chr) return false;
    auto world = ResolveWorldChrMan(SharedConfig());
    if (!world) return false;
    uintptr_t local = 0;
    if (!mem::ReadPtr(*world + kLocalPlayerOff, local) || !local) return false;
    return chr == local;
}

// ---------------------------------------------------------------------------
// Kill rewards. Debug-created enemies never pay out on their own, so each PC
// gives its own player what the enemy's params say it drops.

struct Reward {
    uint64_t handle = 0;
    std::string key;
    int npc = 0;
    int model = 0;
};

std::mt19937& Rng() {
    static std::mt19937 rng{std::random_device{}()};
    return rng;
}

int NpcRunes(int npc) {
    if (npc <= 0) return 0;
    const uintptr_t row = params::Row(params::Table::Npc, static_cast<uint32_t>(npc));
    if (!row) return 0;
    uint32_t souls = 0;
    if (!mem::Read(row + kNpcGetSoul, souls) || souls > 10000000u) return 0;
    return static_cast<int>(souls);
}

unsigned LotItemId(int category, int id) {
    switch (category) {
        case 1: return 0x40000000u | static_cast<unsigned>(id);  // goods
        case 2: return static_cast<unsigned>(id);                // weapon
        case 3: return 0x10000000u | static_cast<unsigned>(id);  // protector
        case 4: return 0x20000000u | static_cast<unsigned>(id);  // accessory
        case 5: return 0x80000000u | static_cast<unsigned>(id);  // ash of war
        default: return 0;
    }
}

// An enemy's drop table is a run of consecutive ItemLotParam_enemy rows, each
// rolled on its own (e.g. 227000000 crab eggs, 227000001 white flesh).
int RollEnemyLots(const Reward& r) {
    int lot = 0;
    if (r.npc > 0) {
        if (const uintptr_t row = params::Row(params::Table::Npc, static_cast<uint32_t>(r.npc))) {
            mem::Read(row + kNpcItemLotEnemy, lot);
        }
    }
    if (lot <= 0 && r.model > 0) lot = r.model * 100000;
    if (lot <= 0) return 0;

    int given = 0;
    for (int k = 0; k < 8; ++k) {
        const uintptr_t row = params::Row(params::Table::ItemLotEnemy, static_cast<uint32_t>(lot + k));
        if (!row) break;
        int ids[8]{}, cats[8]{};
        uint16_t pts[8]{};
        uint8_t nums[8]{};
        if (!mem::ReadBytes(row + kLotIds, ids, sizeof(ids)) ||
            !mem::ReadBytes(row + kLotCategories, cats, sizeof(cats)) ||
            !mem::ReadBytes(row + kLotPoints, pts, sizeof(pts)) ||
            !mem::ReadBytes(row + kLotCounts, nums, sizeof(nums))) {
            break;
        }
        int total = 0;
        for (uint16_t p : pts) total += p;
        if (total <= 0) continue;
        int roll = static_cast<int>(Rng()() % static_cast<unsigned>(total));
        int pick = -1;
        for (int i = 0; i < 8; ++i) {
            if (roll < pts[i]) {
                pick = i;
                break;
            }
            roll -= pts[i];
        }
        if (pick < 0 || ids[pick] <= 0) continue;
        const unsigned item = LotItemId(cats[pick], ids[pick]);
        if (!item) continue;
        const unsigned qty = nums[pick] > 0 ? nums[pick] : 1;
        if (player::GiveItem(item, qty)) {
            ++given;
            log::Info("Drop " + r.key + ": lot " + std::to_string(lot + k) + " item 0x" + HexU(item) +
                      " x" + std::to_string(qty));
        }
    }
    return given;
}

void GiveRewards(std::vector<Reward> list) {
    std::thread([list = std::move(list)]() {
        Config cfg;
        LoadConfig(cfg);
        const int before = player::ReadRunes(cfg);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        const int after = player::ReadRunes(cfg);
        int native = 0;
        if (before >= 0 && after >= before) native = after - before;

        int need = 0;
        for (const auto& r : list) {
            const int table = player::GiveKillLoot(r.key);
            int runes = NpcRunes(r.npc);
            if (runes <= 0) runes = table;
            if (runes > 0) need += runes;
            RollEnemyLots(r);
            log::Info("Kill reward " + r.key + " npc=" + std::to_string(r.npc) + " runes=" +
                      std::to_string(runes) + " (this PC)");
        }
        if (need > native) player::AddRunes(cfg, need - native);
    }).detach();
}

// ---------------------------------------------------------------------------
// Shared HP and posture. Every PC fights its own copy of the enemy. Damage on
// any copy is reported as a fraction of max HP; the host keeps the shared
// fraction and every PC lowers its copy to match.

struct SyncedEnt {
    uint64_t handle = 0;
    uintptr_t chr = 0;
    uintptr_t last_chr = 0;
    std::string key;
    int npc = 0;
    int model = 0;
    Vec3 home{};
    bool have_home = false;

    float shared = 1.f;
    int last_hp = -1;
    float last_sa = -1.f;
    int saved_mx = 0;
    bool seen_alive = false;
    bool dead = false;
    int dead_echo = 0;
    bool rewarded = false;
    int absent = 0;
    bool dirty = false;
    std::chrono::steady_clock::time_point next_bcast{};

    int primed_mx = 0;
    std::chrono::steady_clock::time_point mx_stable_since{};
    std::chrono::steady_clock::time_point born{};
    std::chrono::steady_clock::time_point full_hp_until{};
};

std::mutex g_ents_mu;
std::vector<SyncedEnt> g_ents;
std::atomic<bool> g_sync_started{false};

uint32_t MyOrigin() {
    static const uint32_t id = [] {
        std::random_device rd;
        uint32_t v = 0;
        while (v == 0) v = rd();
        return v;
    }();
    return net::IsClient() ? id : 0u;
}

enum class Life { Up, Dead, Missing };

Life LocalLife() {
    auto world = ResolveWorldChrMan(SharedConfig());
    if (!world) return Life::Missing;
    uintptr_t local = 0;
    if (!mem::ReadPtr(*world + kLocalPlayerOff, local) || !local) return Life::Missing;
    uintptr_t data = 0;
    if (!CharData(local, data)) return Life::Missing;
    int hp = 0, mx = 0;
    if (!mem::Read(data + kHpCur, hp)) return Life::Missing;
    mx = HighestMaxHp(data);
    // Stale WorldChrMan during a grace rest can look like HP 0. A real player
    // max is hundreds to tens of thousands; junk reads get treated as Missing.
    if (mx < 200 || mx > 50000 || hp < 0 || hp > 500000) return Life::Missing;
    return hp <= 0 ? Life::Dead : Life::Up;
}

// A boss loading for the first time reads HP 0 and model id 0 for a while.
// Dropping the pointer then leaves the friend's copy unlinked for good.
bool Fresh(const SyncedEnt& e) {
    return std::chrono::steady_clock::now() - e.born < std::chrono::seconds(10);
}

bool HpLock(const SyncedEnt& e) {
    return std::chrono::steady_clock::now() < e.full_hp_until;
}

void NoteSpawnHp(SyncedEnt& e, int mx) {
    if (mx <= 0) return;
    const auto now = std::chrono::steady_clock::now();
    if (mx > e.primed_mx) {
        e.primed_mx = mx;
        e.mx_stable_since = now;
        // Max is still climbing: keep filling current HP to it.
        e.full_hp_until = std::min(e.born + std::chrono::seconds(4), now + std::chrono::milliseconds(800));
        return;
    }
    if (e.mx_stable_since.time_since_epoch().count() == 0) e.mx_stable_since = now;
}

SyncedEnt* FindEnt(uint64_t handle) {
    for (auto& e : g_ents) {
        if (e.handle == handle) return &e;
    }
    return nullptr;
}

void ApplyPostureLocked(SyncedEnt& e, float frac) {
    if (!e.chr || e.dead || HpLock(e) || frac <= 0.f) return;
    uintptr_t sa = 0;
    float cur = 0.f, mx = 0.f;
    if (!ReadPostureSeh(e.chr, sa, cur, mx)) return;
    const float next = std::max(0.f, cur - frac * mx);
    mem::Write(sa + kSaCur, next);
    e.last_sa = next;
}

void SyncThread();

void EnsureSyncThread() {
    if (g_sync_started.exchange(true)) return;
    std::thread(SyncThread).detach();
}

void Track(uint64_t handle, uintptr_t chr, const std::string& key, const SpawnDef& def, const Vec3& pos,
           bool refill) {
    if (!handle) return;
    std::lock_guard lock(g_ents_mu);
    SyncedEnt* e = FindEnt(handle);
    if (!e) {
        g_ents.push_back({});
        e = &g_ents.back();
        e->handle = handle;
    }
    e->chr = chr;
    e->last_chr = chr;
    e->key = key;
    e->npc = def.npc;
    e->model = def.model;
    e->home = pos;
    e->have_home = true;
    e->dead = false;
    e->dead_echo = 0;
    e->absent = 0;
    e->seen_alive = false;
    e->rewarded = false;
    e->last_hp = -1;
    e->last_sa = -1.f;
    e->primed_mx = 0;
    e->mx_stable_since = {};
    e->born = std::chrono::steady_clock::now();
    if (refill) {
        e->shared = 1.f;
        e->full_hp_until = e->born + std::chrono::milliseconds(800);
    } else {
        // Death restore: keep the shared fraction, do not refill.
        e->full_hp_until = e->born;
    }
    EnsureSyncThread();
    log::Info("Shared HP tracking " + key + " handle=" + HexU(static_cast<uintptr_t>(handle)) +
              (refill ? " (full)" : " (restore)"));
}

uintptr_t CreateOneAt(uintptr_t world, const SpawnDef& def, const Vec3& pos);

void TickSyncedOnce() {
    struct Restore {
        uint64_t handle = 0;
        std::string key;
        int npc = 0;
        int model = 0;
        Vec3 pos{};
    };
    std::vector<std::pair<uint64_t, float>> hp_out;       // client -> host damage
    std::vector<std::pair<uint64_t, float>> posture_out;  // our own posture damage
    std::vector<std::pair<uint64_t, float>> shared_out;   // host -> everyone
    std::vector<Reward> kill_out;                         // notify peers this handle died
    std::vector<Restore> restore;
    std::vector<Reward> rewards;

    const Life life = LocalLife();
    const bool up = life == Life::Up;
    // Death is HP 0 for a stretch of ticks. A grace rest keeps you alive but
    // WorldChrMan can look like HP 0 for a frame or two — that is not a death.
    static int dead_hold = 0;
    static bool confirmed_death = false;
    if (life == Life::Dead) {
        if (++dead_hold >= 45) confirmed_death = true;
    } else {
        dead_hold = 0;
    }
    bool do_rest = false;
    static int rest_hold = 0;
    static uintptr_t last_world = 0;
    static uintptr_t last_player = 0;
    const bool client = net::IsClient();
    const auto now = std::chrono::steady_clock::now();
    uintptr_t world_ptr = 0;
    if (auto w = ResolveWorldChrMan(SharedConfig())) world_ptr = *w;
    uintptr_t player_ptr = 0;
    if (world_ptr) mem::ReadPtr(world_ptr + kLocalPlayerOff, player_ptr);

    {
        std::lock_guard lock(g_ents_mu);
        if (g_ents.empty()) {
            rest_hold = 0;
            last_world = world_ptr;
            last_player = player_ptr;
            return;
        }
        int live = 0, gone = 0, tracked = 0;
        for (auto it = g_ents.begin(); it != g_ents.end();) {
            SyncedEnt& e = *it;
            if (e.chr) e.last_chr = e.chr;
            if (e.chr && IsLocalPlayer(e.chr)) e.chr = 0;
            if (e.chr && e.model != 0 && !Fresh(e)) {
                int id = 0;
                if (mem::Read(e.chr + kChrIdOffset, id)) {
                    if (id > 100000) id /= 10000;
                    if (id == 0) {
                        log::Warn("Shared HP link lost for " + e.key + " (model id 0)");
                        e.chr = 0;
                    }
                }
            }

            // While we are dead or reloading every tracked pointer may already be freed.
            if (up) {
                bool present = false;
                int hp = 0, mx = 0;
                if (e.chr && HpLock(e)) {
                    NoteSpawnHp(e, ForceFullHpSeh(e.chr));
                    if (ReadHpSeh(e.chr, hp, mx)) {
                        e.last_hp = hp;
                        if (hp > 0) e.seen_alive = true;
                    }
                    uintptr_t sa = 0;
                    float cur = 0.f, smx = 0.f;
                    if (ReadPostureSeh(e.chr, sa, cur, smx)) e.last_sa = cur;
                    present = true;
                    e.absent = 0;
                } else if (e.chr && ReadHpSeh(e.chr, hp, mx)) {
                    present = true;
                    e.absent = 0;
                    if (!e.dead) {
                        if (e.last_hp >= 0 && hp < e.last_hp) {
                            const float d = static_cast<float>(e.last_hp - hp) / static_cast<float>(mx);
                            e.shared = std::max(0.f, e.shared - d);
                            if (client) hp_out.emplace_back(e.handle, d);
                            else e.dirty = true;
                        }
                        const int target = e.shared <= 0.f
                                               ? 0
                                               : std::max(1, static_cast<int>(std::lround(e.shared * mx)));
                        if (hp != target && WriteHpSeh(e.chr, target)) hp = target;
                        e.last_hp = hp;

                        uintptr_t sa = 0;
                        float cur = 0.f, smx = 0.f;
                        if (ReadPostureSeh(e.chr, sa, cur, smx)) {
                            if (e.last_sa >= 0.f && cur < e.last_sa) {
                                posture_out.emplace_back(e.handle, (e.last_sa - cur) / smx);
                            }
                            e.last_sa = cur;
                        }

                        if (hp > 0) {
                            e.seen_alive = true;
                            e.saved_mx = mx;
                            if (auto pos = ReadPosFromChr(e.chr)) {
                                e.home = *pos;
                                e.have_home = true;
                            }
                        } else if (e.seen_alive) {
                            e.dead = true;
                            e.shared = 0.f;
                            if (!client) e.dirty = true;
                            if (!e.rewarded) {
                                e.rewarded = true;
                                rewards.push_back({e.handle, e.key, e.npc, e.model});
                                kill_out.push_back({e.handle, e.key, e.npc, e.model});
                                log::Info("Shared enemy down: " + e.key + " — loot to every PC");
                            }
                        }
                    }
                } else if (!e.dead) {
                    ++e.absent;
                    // Keep the pointer across a short miss. Nulling it immediately
                    // made the next ticks fail forever and looked like a grace rest.
                    if (e.absent >= 40 && !Fresh(e)) e.chr = 0;
                }
                if (present && !e.dead) ++live;
                if (!e.dead && e.seen_alive) {
                    ++tracked;
                    if (!present) ++gone;
                }

                // Map reload after a death deletes the body. Put the same fight back.
                if (confirmed_death && !e.dead && !e.chr && e.absent >= 40 && e.have_home &&
                    e.saved_mx > 0 && e.shared > 0.f) {
                    restore.push_back({e.handle, e.key, e.npc, e.model, e.home});
                    e.absent = 0;
                }
            }

            if (!client && (e.dirty || now >= e.next_bcast)) {
                shared_out.emplace_back(e.handle, e.shared);
                e.dirty = false;
                e.next_bcast = now + std::chrono::milliseconds(500);
            }

            const bool never_spawned = !e.chr && !e.seen_alive && now - e.born > std::chrono::seconds(30);
            if ((e.dead && ++e.dead_echo >= 40) || never_spawned) {
                it = g_ents.erase(it);
                continue;
            }
            ++it;
        }
        if (up && live > 0) confirmed_death = false;
        const bool world_reloaded =
            up && !confirmed_death && tracked > 0 && live == 0 &&
            ((last_world && world_ptr && world_ptr != last_world) ||
             (last_player && player_ptr && player_ptr != last_player));
        if (!confirmed_death && up && tracked > 0 && live == 0 && gone >= tracked) {
            ++rest_hold;
        } else {
            rest_hold = 0;
        }
        if (world_reloaded || rest_hold >= 45) {
            do_rest = true;
            rest_hold = 0;
            log::Info(std::string("Rest detected (") + (world_reloaded ? "reload" : "enemies gone") +
                      ") — despawning " + std::to_string(tracked) + " tracked");
        }
        last_world = world_ptr;
        last_player = player_ptr;
    }

    if (!rewards.empty()) GiveRewards(std::move(rewards));

    if (net::IsClient() || net::ClientCount() > 0) {
        char buf[256];
        for (const auto& [h, d] : hp_out) {
            std::snprintf(buf, sizeof(buf), "{\"cmd\":\"hpd\",\"h\":\"%llX\",\"d\":%.6f}",
                          static_cast<unsigned long long>(h), d);
            net::SendToHost(buf);
        }
        for (const auto& [h, p] : posture_out) {
            std::snprintf(buf, sizeof(buf), "{\"cmd\":\"pd\",\"h\":\"%llX\",\"p\":%.6f,\"o\":%u}",
                          static_cast<unsigned long long>(h), p, MyOrigin());
            if (client) net::SendToHost(buf);
            else net::Broadcast(buf);
        }
        for (const auto& [h, f] : shared_out) {
            std::snprintf(buf, sizeof(buf), "{\"cmd\":\"hp\",\"h\":\"%llX\",\"f\":%.6f}",
                          static_cast<unsigned long long>(h), f);
            net::Broadcast(buf);
        }
        for (const auto& k : kill_out) {
            std::snprintf(buf, sizeof(buf),
                          "{\"cmd\":\"kill\",\"h\":\"%llX\",\"k\":\"%s\",\"n\":%d,\"m\":%d}",
                          static_cast<unsigned long long>(k.handle), k.key.c_str(), k.npc, k.model);
            if (client) net::SendToHost(buf);
            else net::Broadcast(buf);
        }
    }

    if (do_rest) {
        ClearSpawned(true);
        return;
    }

    for (const auto& job : restore) {
        auto world = ResolveWorldChrMan(SharedConfig());
        if (!world) continue;
        const auto defs = Catalog(job.key);
        if (defs.empty()) continue;
        SpawnDef def = defs[0];
        for (const auto& d : defs) {
            if (d.npc == job.npc) def = d;
        }
        log::Info("Respawn restore " + job.key);
        const uintptr_t chr = CreateOneAt(*world, def, job.pos);
        if (chr) Track(job.handle, chr, job.key, def, job.pos, false);
    }
}

void SyncThread() {
    while (true) {
        TickSyncedOnce();
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
}

void RememberTracked(std::unordered_set<uintptr_t>& before) {
    std::lock_guard lock(g_ents_mu);
    for (const auto& e : g_ents) {
        if (e.chr) before.insert(e.chr);
        if (e.last_chr) before.insert(e.last_chr);
    }
}

void ForgetChr(uintptr_t chr) {
    if (!chr) return;
    std::lock_guard lock(g_ents_mu);
    for (auto& e : g_ents) {
        if (e.chr != chr && e.last_chr != chr) continue;
        e.chr = 0;
        e.last_chr = 0;
    }
}

uintptr_t FindNewChr(uintptr_t world, const std::unordered_set<uintptr_t>& before,
                     const std::unordered_set<int>& wanted, const Vec3& at) {
    uintptr_t best_match = 0;
    float match_d = 1e30f;
    for (uintptr_t ent : CollectEntities(world)) {
        if (before.count(ent) || IsLocalPlayer(ent)) continue;
        int id = 0;
        if (!mem::Read(ent + kChrIdOffset, id) || !IdInWanted(id, wanted)) continue;
        auto pos = ReadPosFromChr(ent);
        if (!pos) continue;
        const float d = Dist2(*pos, at);
        if (d > 80.f * 80.f) continue;
        if (d < match_d) {
            match_d = d;
            best_match = ent;
        }
    }
    return best_match;
}

uintptr_t CreateOneAt(uintptr_t world, const SpawnDef& def, const Vec3& pos) {
    std::unordered_set<uintptr_t> before;
    std::unordered_map<uintptr_t, Vec3> was;
    for (uintptr_t e : CollectEntities(world)) {
        before.insert(e);
        if (auto p = ReadPosFromChr(e)) was[e] = *p;
    }
    RememberTracked(before);

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

    const auto wanted = ModelsOf({def});
    uintptr_t chr = 0;
    // Boss models show up late. Keep looking for a pointer we did not already track,
    // instead of grabbing the previous Margit.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(cleared ? 2000 : 2500);
    while (std::chrono::steady_clock::now() < deadline) {
        chr = FindNewChr(world, before, wanted, pos);
        if (chr) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    bool reused = false;
    if (!chr) {
        // The game sometimes moves the existing body to the new point instead of
        // allocating another one. Take it only if it actually moved there.
        for (uintptr_t ent : CollectEntities(world)) {
            if (!before.count(ent) || IsLocalPlayer(ent)) continue;
            int id = 0;
            if (!mem::Read(ent + kChrIdOffset, id) || !IdInWanted(id, wanted)) continue;
            auto pos_now = ReadPosFromChr(ent);
            if (!pos_now || Dist2(*pos_now, pos) > 15.f * 15.f) continue;
            auto old = was.find(ent);
            if (old != was.end() && Dist2(old->second, pos) <= 15.f * 15.f) continue;
            chr = ent;
            reused = true;
            break;
        }
    }
    if (!chr) {
        log::Warn("CREATE no new chr for c" + std::to_string(def.model));
        return 0;
    }
    if (reused) ForgetChr(chr);
    log::Info("CREATE ok c" + std::to_string(def.model) + " npc=" + std::to_string(def.npc) +
              " chr=0x" + HexU(chr) + " flagCleared=" + (cleared ? "1" : "0"));
    EnsureAliveHostile(chr);
    return chr;
}

int TryCreateNear(uintptr_t world, const Vec3& at, const std::vector<SpawnDef>& defs, int count,
                  const std::string& enemy_key) {
    if (g_create_disabled || defs.empty()) return 0;

    int created = 0;
    for (int i = 0; i < count; ++i) {
        const SpawnDef& def = defs[static_cast<size_t>(i) % defs.size()];
        const float angle = (6.2831853f / static_cast<float>(count)) * static_cast<float>(i);
        Vec3 pos{at.x + std::cos(angle) * 4.0f, at.y + 0.5f, at.z + std::sin(angle) * 4.0f};
        uintptr_t chr = CreateOneAt(world, def, pos);
        if (!chr) continue;
        static std::atomic<uint32_t> local_seq{1};
        const uint64_t handle = 0xC4A0510000000000ull | local_seq.fetch_add(1);
        Track(handle, chr, enemy_key, def, pos, true);
        ++created;
    }
    return created;
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
        if (ResolveChrSp(*world)) {
            n = TryCreateNear(*world, *pos, defs, count, enemy_key);
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
    log::Info("Spawn " + enemy_key + " x" + std::to_string(count) + " at fixed position");

    int n = 0;
    if (ResolveChrSp(*world)) n = TryCreateNear(*world, pos, defs, count, enemy_key);
    if (n <= 0) {
        log::Error("Fixed-position spawn failed for " + enemy_key);
        return false;
    }
    log::Info("Fixed-position spawn ok total=" + std::to_string(n) + " key=" + enemy_key);
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
        uintptr_t chr = CreateOneAt(*world, def, pos);
        if (!chr) continue;
        Track(handles[i], chr, enemy_key, def, pos, true);
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

void OnRemoteDamage(uint64_t handle, float hp_frac) {
    if (net::IsClient() || !handle || !(hp_frac > 0.f)) return;
    std::lock_guard lock(g_ents_mu);
    SyncedEnt* e = FindEnt(handle);
    if (!e || HpLock(*e)) return;
    e->shared = std::max(0.f, e->shared - std::min(hp_frac, 1.f));
    e->dirty = true;
}

void OnSharedHp(uint64_t handle, float frac) {
    if (!net::IsClient() || !handle || !std::isfinite(frac)) return;
    frac = std::clamp(frac, 0.f, 1.f);
    std::lock_guard lock(g_ents_mu);
    SyncedEnt* e = FindEnt(handle);
    if (!e) {
        if (frac <= 0.f) return;
        // The host's first update can beat our own create.
        g_ents.push_back({});
        e = &g_ents.back();
        e->handle = handle;
        e->born = std::chrono::steady_clock::now();
        e->full_hp_until = e->born + std::chrono::milliseconds(800);
        e->shared = 1.f;
        EnsureSyncThread();
    }
    if (HpLock(*e)) return;
    e->shared = std::min(e->shared, frac);
}

void OnRemotePosture(uint64_t handle, float posture_frac, uint32_t origin) {
    if (!handle || !(posture_frac > 0.f) || !std::isfinite(posture_frac)) return;
    if (origin != 0 && origin == MyOrigin()) return;
    {
        std::lock_guard lock(g_ents_mu);
        if (SyncedEnt* e = FindEnt(handle)) ApplyPostureLocked(*e, std::min(posture_frac, 1.f));
    }
    if (!net::IsClient() && net::ClientCount() > 0) {
        char buf[160];
        std::snprintf(buf, sizeof(buf), "{\"cmd\":\"pd\",\"h\":\"%llX\",\"p\":%.6f,\"o\":%u}",
                      static_cast<unsigned long long>(handle), posture_frac, origin);
        net::Broadcast(buf);
    }
}

void OnRemoteKill(uint64_t handle, const std::string& key, int npc, int model) {
    if (!handle) return;
    Reward pay;
    {
        std::lock_guard lock(g_ents_mu);
        SyncedEnt* e = FindEnt(handle);
        if (!e) {
            g_ents.push_back({});
            e = &g_ents.back();
            e->handle = handle;
            e->born = std::chrono::steady_clock::now();
            EnsureSyncThread();
        }
        if (e->rewarded) return;
        e->rewarded = true;
        e->dead = true;
        e->shared = 0.f;
        if (!key.empty()) e->key = key;
        if (npc) e->npc = npc;
        if (model) e->model = model;
        pay = {e->handle, e->key, e->npc, e->model};
    }
    log::Info("Kill loot from peer: " + pay.key);
    GiveRewards({pay});
    if (!net::IsClient() && net::ClientCount() > 0) {
        char buf[256];
        std::snprintf(buf, sizeof(buf),
                      "{\"cmd\":\"kill\",\"h\":\"%llX\",\"k\":\"%s\",\"n\":%d,\"m\":%d}",
                      static_cast<unsigned long long>(pay.handle), pay.key.c_str(), pay.npc, pay.model);
        net::Broadcast(buf);
    }
}

void ClearSpawned(bool tell_peer) {
    std::vector<uintptr_t> chrs;
    {
        std::lock_guard lock(g_ents_mu);
        for (const auto& e : g_ents) {
            if (e.chr) chrs.push_back(e.chr);
        }
        g_ents.clear();
    }
    for (uintptr_t chr : chrs) WriteHpSeh(chr, 0);
    log::Info("Rest: despawned " + std::to_string(chrs.size()) + " tracked enemies");
    if (!tell_peer) return;
    const std::string msg = "{\"cmd\":\"rest\"}";
    if (net::IsClient()) net::SendToHost(msg);
    else if (net::ClientCount() > 0) net::Broadcast(msg);
}

uintptr_t LocalPlayerChr() {
    auto world = ResolveWorldChrMan(SharedConfig());
    if (!world) return 0;
    uintptr_t local = 0;
    if (!mem::ReadPtr(*world + kLocalPlayerOff, local)) return 0;
    return local;
}

}  // namespace chaos::spawner
