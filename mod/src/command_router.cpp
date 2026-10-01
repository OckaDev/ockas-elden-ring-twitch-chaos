#include "command_router.hpp"
#include "game/input_fx.hpp"
#include "game/player.hpp"
#include "game/rtti_dump.hpp"
#include "game/spawner.hpp"
#include "game/targets.hpp"
#include "net_sync.hpp"
#include "util/config.hpp"
#include "util/log.hpp"

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <optional>
#include <string>
#include <vector>

namespace chaos {
namespace {

std::optional<std::string> JsonString(const std::string& json, const std::string& key) {
    // Match "key": "value" only — not a value that happens to equal the key name.
    const std::string needle = "\"" + key + "\"";
    size_t pos = 0;
    while ((pos = json.find(needle, pos)) != std::string::npos) {
        size_t after = pos + needle.size();
        while (after < json.size() && (json[after] == ' ' || json[after] == '\t')) ++after;
        if (after >= json.size() || json[after] != ':') {
            pos = after;
            continue;
        }
        ++after;
        while (after < json.size() && (json[after] == ' ' || json[after] == '\t')) ++after;
        if (after >= json.size() || json[after] != '"') return std::nullopt;
        const size_t start = after + 1;
        const size_t end = json.find('"', start);
        if (end == std::string::npos) return std::nullopt;
        return json.substr(start, end - start);
    }
    return std::nullopt;
}

std::optional<int> JsonInt(const std::string& json, const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    size_t pos = 0;
    while ((pos = json.find(needle, pos)) != std::string::npos) {
        size_t after = pos + needle.size();
        while (after < json.size() && (json[after] == ' ' || json[after] == '\t')) ++after;
        if (after >= json.size() || json[after] != ':') {
            pos = after;
            continue;
        }
        ++after;
        while (after < json.size() && (json[after] == ' ' || json[after] == '\t')) ++after;
        try {
            return std::stoi(json.substr(after));
        } catch (...) {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

std::optional<float> JsonFloat(const std::string& json, const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    size_t pos = 0;
    while ((pos = json.find(needle, pos)) != std::string::npos) {
        size_t after = pos + needle.size();
        while (after < json.size() && (json[after] == ' ' || json[after] == '\t')) ++after;
        if (after >= json.size() || json[after] != ':') {
            pos = after;
            continue;
        }
        ++after;
        while (after < json.size() && (json[after] == ' ' || json[after] == '\t')) ++after;
        try {
            return std::stof(json.substr(after));
        } catch (...) {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

targets::Mode TargetFromJson(const std::string& json) {
    auto t = JsonString(json, "target");
    if (!t) return targets::Mode::All;
    return targets::ParseMode(*t);
}

int RandomPm5() {
    static bool seeded = false;
    if (!seeded) {
        std::srand(static_cast<unsigned>(std::time(nullptr)) ^ 0x5A17u);
        seeded = true;
    }
    return (std::rand() % 2 == 0) ? 5 : -5;
}

int ClampSpawnCount(int n) {
    if (n < 1) return 1;
    if (n > 12) return 12;
    return n;
}

int ClampBossCount(int n) {
    if (n < 1) return 1;
    return n;
}

bool SpawnBosses(const Config& cfg, const std::string& key, int count, targets::Mode mode) {
    count = ClampBossCount(count);
    int ok = 0;
    for (int i = 0; i < count; ++i) {
        if (spawner::SpawnNearTargets(cfg, key, 1, mode)) ++ok;
    }
    log::Info("Boss batch " + std::to_string(ok) + "/" + std::to_string(count) + " key=" + key);
    return ok > 0;
}

std::string Lower(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

std::vector<uint64_t> ParseHandles(const std::string& s) {
    std::vector<uint64_t> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (s[i] == ',' || s[i] == ' ')) ++i;
        if (i >= s.size()) break;
        size_t j = i;
        while (j < s.size() && s[j] != ',' && s[j] != ' ') ++j;
        try {
            out.push_back(std::stoull(s.substr(i, j - i), nullptr, 16));
        } catch (...) {
        }
        i = j;
    }
    return out;
}

// Runs a command against this PC. `force_local` = only touch our own player.
void Execute(const std::string& json, bool force_local);

}  // namespace

void HandleRemoteCommandJson(const std::string& json) {
    const auto c = Lower(JsonString(json, "cmd").value_or(""));
    if (c == "rest") {
        log::Info("Net rest: despawn tracked enemies");
        spawner::ClearSpawned(false);
        return;
    }
    if (c == "hpd" || c == "hp" || c == "pd") {
        uint64_t handle = 0;
        try {
            handle = std::stoull(JsonString(json, "h").value_or(""), nullptr, 16);
        } catch (...) {
            return;
        }
        if (c == "hpd") {
            if (auto d = JsonFloat(json, "d")) spawner::OnRemoteDamage(handle, *d);
        } else if (c == "hp") {
            if (auto f = JsonFloat(json, "f")) spawner::OnSharedHp(handle, *f);
        } else if (auto p = JsonFloat(json, "p")) {
            // Origin ids use the full u32 range, too big for JsonInt.
            uint32_t origin = 0;
            const size_t at = json.find("\"o\":");
            if (at != std::string::npos) {
                try {
                    origin = static_cast<uint32_t>(std::stoul(json.substr(at + 4)));
                } catch (...) {
                }
            }
            spawner::OnRemotePosture(handle, *p, origin);
        }
        return;
    }
    log::Info("Net payload: " + json);
    Execute(json, true);
}

void HandleCommandJson(const std::string& json_in) {
    log::Info("UDP payload: " + json_in);
    std::string json = json_in;

    // "Drain or refill" is decided once here so every PC gets the same result.
    {
        const auto eff = Lower(JsonString(json, "effect").value_or(""));
        const auto m = Lower(JsonString(json, "mode").value_or(""));
        if (eff == "flasks" && m != "drain" && m != "refill") {
            static bool seeded = false;
            if (!seeded) {
                std::srand(static_cast<unsigned>(std::time(nullptr)) ^ 0xF1A5u);
                seeded = true;
            }
            const char* pick = (std::rand() % 2 == 0) ? "drain" : "refill";
            json = std::string("{\"cmd\":\"effect\",\"effect\":\"flasks\",\"mode\":\"") + pick +
                   "\",\"target\":\"" + JsonString(json_in, "target").value_or("all") + "\"}";
            log::Info(std::string("Flasks roll: ") + pick);
        }
    }

    // Multiplayer link: friends run the mod too, so each PC only edits its own player.
    const int friends = net::ClientCount();
    if (friends > 0 && !net::IsClient()) {
        const auto target = TargetFromJson(json);

        // Spawns: every PC creates the requested enemy (lobster, boss, ...) at the
        // same coordinates, then claims the same runtime handle Seamless uses for
        // Enemy Rush so both of you fight that type — not a recycled map soldier.
        {
            const auto cmdName = Lower(JsonString(json, "cmd").value_or(""));
            const auto effName = Lower(JsonString(json, "effect").value_or(""));
            const bool isBoss = (effName == "boss" || effName == "boss_random");
            const bool isSpawn = (cmdName == "spawn") || effName == "spawn" || isBoss;
            if (isSpawn) {
                Config cfg;
                LoadConfig(cfg);
                static bool seeded = false;
                if (!seeded) {
                    std::srand(static_cast<unsigned>(std::time(nullptr)) ^ 0x51DEu);
                    seeded = true;
                }
                int pick = (target == targets::Mode::Random)
                               ? (std::rand() % (friends + 1))
                               : 0;
                float x = 0, y = 0, z = 0;
                bool have_pos = spawner::PickParticipantPosition(cfg, pick, x, y, z);
                // Offline same-PC test: the friend copy is linked but not in our world.
                if (!have_pos && pick != 0) {
                    pick = 0;
                    have_pos = spawner::PickParticipantPosition(cfg, pick, x, y, z);
                }
                if (have_pos) {
                    std::string enemy = JsonString(json, "boss").value_or(
                        JsonString(json, "enemy").value_or(isBoss ? "boss_random" : ""));
                    if (!enemy.empty()) {
                        const int count = isBoss
                            ? ClampBossCount(JsonInt(json, "count").value_or(1))
                            : ClampSpawnCount(JsonInt(json, "count").value_or(1));
                        for (int n = 0; n < count; ++n) {
                            // boss_random re-rolls each time so a 5-gift is five different bosses.
                            const std::string one = spawner::ResolveKey(enemy);
                            std::vector<uint64_t> handles;
                            spawner::ReserveRuntimeHandles(cfg, 1, handles);
                            std::string hs;
                            for (size_t i = 0; i < handles.size(); ++i) {
                                if (i) hs += ",";
                                char hb[24];
                                std::snprintf(hb, sizeof(hb), "%llX",
                                              static_cast<unsigned long long>(handles[i]));
                                hs += hb;
                            }
                            char buf[640];
                            if (!hs.empty()) {
                                std::snprintf(buf, sizeof(buf),
                                              "{\"cmd\":\"effect\",\"effect\":\"spawn\","
                                              "\"enemy\":\"%s\",\"handles\":\"%s\","
                                              "\"x\":%.3f,\"y\":%.3f,\"z\":%.3f,"
                                              "\"sync\":3,\"count\":1}",
                                              one.c_str(), hs.c_str(), x, y, z);
                            } else {
                                std::snprintf(buf, sizeof(buf),
                                              "{\"cmd\":\"effect\",\"effect\":\"spawn\","
                                              "\"enemy\":\"%s\",\"count\":1,"
                                              "\"x\":%.3f,\"y\":%.3f,\"z\":%.3f,\"sync\":1}",
                                              one.c_str(), x, y, z);
                            }
                            const std::string payload = buf;
                            log::Info("Net: coop create " + one + " near " +
                                      std::string(pick == 0 ? "host" : "friend " + std::to_string(pick)) +
                                      " -> everyone (" + std::to_string(n + 1) + "/" +
                                      std::to_string(count) + ")");
                            net::Broadcast(payload);
                            Execute(payload, true);
                        }
                        return;
                    }
                } else {
                    log::Warn("Net: could not read target position, using per-player spawn");
                }
            }
        }

        if (target == targets::Mode::Random) {
            static bool seeded = false;
            if (!seeded) {
                std::srand(static_cast<unsigned>(std::time(nullptr)) ^ 0x7E11u);
                seeded = true;
            }
            const int pick = std::rand() % (friends + 1);  // 0 = host
            if (pick == 0) {
                log::Info("Net: random pick = host");
                Execute(json, true);
            } else {
                log::Info("Net: random pick = friend " + std::to_string(pick));
                if (!net::SendToClient(pick - 1, json)) {
                    log::Warn("Net: friend gone, running on host instead");
                    Execute(json, true);
                }
            }
        } else {
            net::Broadcast(json);
            Execute(json, true);
        }
        return;
    }

    Execute(json, false);
}

namespace {

void Execute(const std::string& json, bool force_local) {
    Config cfg;
    LoadConfig(cfg);

    auto cmd = JsonString(json, "cmd");
    if (!cmd) {
        log::Warn("Missing cmd");
        return;
    }
    const auto c = Lower(*cmd);
    const auto mode = force_local ? targets::Mode::Local : TargetFromJson(json);

    // Coop create: same enemy type on every PC, same runtime handle.
    if (auto hs = JsonString(json, "handles")) {
        if (JsonInt(json, "sync").value_or(0) == 3) {
            const auto fx = JsonFloat(json, "x");
            const auto fy = JsonFloat(json, "y");
            const auto fz = JsonFloat(json, "z");
            auto enemy = JsonString(json, "enemy");
            const auto handles = ParseHandles(*hs);
            if (fx && fy && fz && enemy && !handles.empty()) {
                if (!spawner::SpawnCreatedWithHandles(cfg, *enemy, handles, *fx, *fy, *fz)) {
                    log::Error("coop create failed");
                }
                return;
            }
        }
    }

    // Synced spawn from the host: fixed world position, identical on every PC.
    {
        const auto fx = JsonFloat(json, "x");
        const auto fy = JsonFloat(json, "y");
        const auto fz = JsonFloat(json, "z");
        auto enemy = JsonString(json, "enemy");
        if (fx && fy && fz && enemy && JsonInt(json, "sync").value_or(0) == 1) {
            const int n = JsonInt(json, "count").value_or(1);
            if (!spawner::SpawnAtPosition(cfg, *enemy, n, *fx, *fy, *fz)) {
                log::Error("synced spawn failed");
            }
            return;
        }
    }

    // ---- diagnostics ----
    if (c == "dump_rtti") {
        std::vector<std::string> kws;
        if (auto k = JsonString(json, "keywords")) {
            size_t start = 0;
            while (start <= k->size()) {
                const size_t comma = k->find(',', start);
                const std::string part = k->substr(start, comma == std::string::npos ? std::string::npos : comma - start);
                if (!part.empty()) kws.push_back(part);
                if (comma == std::string::npos) break;
                start = comma + 1;
            }
        }
        if (kws.empty()) kws = {"Generator", "SpawnChr", "ChrSpawn"};
        rtti::DumpClasses(kws, static_cast<size_t>(JsonInt(json, "entries").value_or(24)));
        return;
    }

    // ---- legacy cmds (still supported) ----
    if (c == "heal") {
        if (!player::HealHp(cfg, mode)) log::Error("heal failed");
        return;
    }
    if (c == "spawn") {
        auto enemy = JsonString(json, "enemy");
        int count = JsonInt(json, "count").value_or(3);
        if (!enemy) {
            log::Warn("spawn missing enemy");
            return;
        }
        if (!spawner::SpawnNearTargets(cfg, *enemy, count, mode)) log::Error("spawn failed");
        return;
    }
    if (c == "stat") {
        auto stat = JsonString(json, "stat");
        auto delta = JsonInt(json, "delta");
        if (!stat) {
            log::Warn("stat missing fields");
            return;
        }
        const int d = delta.value_or(RandomPm5());
        if (!player::ApplyStatDelta(cfg, *stat, d, mode)) log::Error("stat failed");
        return;
    }

    // ---- unified effect cmd ----
    if (c == "effect") {
        auto effect = JsonString(json, "effect");
        if (!effect) {
            log::Warn("effect missing effect=");
            return;
        }
        const auto e = Lower(*effect);

        if (e == "heal" || e == "heal_hp") {
            if (!player::HealHp(cfg, mode)) log::Error("effect heal failed");
            return;
        }
        if (e == "fp" || e == "restore_fp" || e == "full_fp") {
            if (!player::RestoreFp(cfg, mode)) log::Error("effect fp failed");
            return;
        }
        if (e == "runes" || e == "lose_runes") {
            const int pct = JsonInt(json, "percent").value_or(10);
            if (!player::LoseRunesPercent(cfg, pct, mode)) log::Error("effect runes failed");
            return;
        }
        if (e == "give_weapon") {
            if (!player::GiveRandomWeapon(cfg, mode)) log::Error("effect give_weapon failed");
            return;
        }
        if (e == "unequip") {
            if (!player::UnequipRandomGear(cfg, mode)) log::Error("effect unequip failed");
            return;
        }
        if (e == "ash_of_war" || e == "random_ash") {
            if (!player::RandomizeAshOfWar(cfg)) log::Error("effect ash_of_war failed");
            return;
        }
        if (e == "swap_hands") {
            if (!player::SwapHands(cfg)) log::Error("effect swap_hands failed");
            return;
        }
        if (e == "flasks" || e == "drain_flasks" || e == "refill_flasks") {
            const auto m = Lower(JsonString(json, "mode").value_or(""));
            const bool fill = e == "refill_flasks" || m == "refill" || m == "fill";
            if (!player::SetFlasks(cfg, fill)) log::Error("effect flasks failed");
            return;
        }
        if (e == "no_roll") {
            inputfx::BlockRoll(JsonInt(json, "ms").value_or(5000));
            return;
        }
        if (e == "invert_move" || e == "invert_movement") {
            inputfx::InvertMovement(JsonInt(json, "ms").value_or(10000));
            return;
        }
        if (e == "stat") {
            auto stat = JsonString(json, "stat");
            if (!stat) {
                log::Warn("effect stat missing stat=");
                return;
            }
            const int d = JsonInt(json, "delta").value_or(RandomPm5());
            if (!player::ApplyStatDelta(cfg, *stat, d, mode)) log::Error("effect stat failed");
            return;
        }
        if (e == "spawn") {
            auto enemy = JsonString(json, "enemy");
            int count = JsonInt(json, "count").value_or(1);
            if (!enemy) {
                log::Warn("effect spawn missing enemy=");
                return;
            }
            if (!spawner::SpawnNearTargets(cfg, *enemy, count, mode)) log::Error("effect spawn failed");
            return;
        }
        if (e == "boss" || e == "boss_random") {
            auto boss = JsonString(json, "boss");
            if (!boss) boss = JsonString(json, "enemy");
            const std::string key = boss ? *boss : "boss_random";
            const int count = ClampBossCount(JsonInt(json, "count").value_or(1));
            if (!SpawnBosses(cfg, key, count, mode)) log::Error("effect boss failed");
            return;
        }

        log::Warn("Unknown effect: " + *effect);
        return;
    }

    log::Warn("Unknown cmd: " + *cmd);
}

}  // namespace
}  // namespace chaos
