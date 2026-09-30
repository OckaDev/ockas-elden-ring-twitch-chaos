#include "util/config.hpp"
#include "util/log.hpp"

#include <fstream>
#include <optional>
#include <sstream>

#include <Windows.h>

namespace chaos {
namespace {

std::string ReadFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Tiny helpers for our fixed JSON shape (no full JSON library dependency).
std::optional<std::string> JsonString(const std::string& json, const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return std::nullopt;
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return std::nullopt;
    pos = json.find('"', pos + 1);
    if (pos == std::string::npos) return std::nullopt;
    auto end = json.find('"', pos + 1);
    if (end == std::string::npos) return std::nullopt;
    return json.substr(pos + 1, end - pos - 1);
}

std::optional<int> JsonInt(const std::string& json, const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return std::nullopt;
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return std::nullopt;
    ++pos;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) ++pos;
    try {
        size_t consumed = 0;
        int v = std::stoi(json.substr(pos), &consumed);
        return v;
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<bool> JsonBool(const std::string& json, const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return std::nullopt;
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return std::nullopt;
    auto truePos = json.find("true", pos);
    auto falsePos = json.find("false", pos);
    if (truePos != std::string::npos && (falsePos == std::string::npos || truePos < falsePos)) return true;
    if (falsePos != std::string::npos) return false;
    return std::nullopt;
}

uintptr_t ParseHexOrDec(const std::string& s) {
    if (s.rfind("0x", 0) == 0 || s.rfind("0X", 0) == 0) {
        return static_cast<uintptr_t>(std::stoull(s, nullptr, 16));
    }
    return static_cast<uintptr_t>(std::stoull(s, nullptr, 10));
}

std::string ExtractObject(const std::string& json, const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return {};
    pos = json.find('{', pos);
    if (pos == std::string::npos) return {};
    int depth = 0;
    for (size_t i = pos; i < json.size(); ++i) {
        if (json[i] == '{') ++depth;
        else if (json[i] == '}') {
            --depth;
            if (depth == 0) return json.substr(pos, i - pos + 1);
        }
    }
    return {};
}

}  // namespace

std::string DllDirectory() {
    char path[MAX_PATH]{};
    HMODULE self = nullptr;
    GetModuleHandleExA(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCSTR>(&DllDirectory),
        &self);
    GetModuleFileNameA(self, path, MAX_PATH);
    std::string p(path);
    auto slash = p.find_last_of("\\/");
    if (slash != std::string::npos) return p.substr(0, slash + 1);
    return ".\\";
}

bool LoadConfig(Config& out) {
    Config cfg;
    const std::string path = DllDirectory() + "config\\signatures.json";
    const std::string json = ReadFile(path);
    if (json.empty()) {
        log::Warn("Could not read config/signatures.json — using defaults");
        out = cfg;
        return false;
    }

    if (auto port = JsonInt(json, "udp_port")) cfg.udp_port = *port;

    auto pointers = ExtractObject(json, "pointers");
    auto world = ExtractObject(pointers, "WorldChrMan");
    auto gdm = ExtractObject(pointers, "GameDataMan");
    if (auto aob = JsonString(world, "aob")) cfg.worldchr_aob = *aob;
    if (auto aob = JsonString(gdm, "aob")) cfg.gamedata_aob = *aob;
    if (auto v = JsonInt(world, "rip_offset_at")) cfg.worldchr_rip_offset_at = *v;
    if (auto v = JsonInt(world, "rip_instruction_size")) cfg.worldchr_rip_size = *v;
    if (auto v = JsonInt(gdm, "rip_offset_at")) cfg.gamedata_rip_offset_at = *v;
    if (auto v = JsonInt(gdm, "rip_instruction_size")) cfg.gamedata_rip_size = *v;

    auto player = ExtractObject(json, "player");
    if (auto s = JsonString(player, "worldchr_player_offset")) cfg.worldchr_player_offset = ParseHexOrDec(*s);
    if (auto s = JsonString(player, "chr_ins_module_base")) cfg.chr_ins_module_base = ParseHexOrDec(*s);
    if (auto s = JsonString(player, "hp_current")) cfg.hp_current = ParseHexOrDec(*s);
    if (auto s = JsonString(player, "hp_max")) cfg.hp_max = ParseHexOrDec(*s);
    if (auto s = JsonString(player, "gamedata_player_game_data")) cfg.gamedata_player_game_data = ParseHexOrDec(*s);

    auto stats = ExtractObject(player, "stats");
    auto loadStat = [&](const char* name, uintptr_t& field) {
        if (auto s = JsonString(stats, name)) field = ParseHexOrDec(*s);
    };
    loadStat("vigor", cfg.stats.vigor);
    loadStat("mind", cfg.stats.mind);
    loadStat("endurance", cfg.stats.endurance);
    loadStat("strength", cfg.stats.strength);
    loadStat("dexterity", cfg.stats.dexterity);
    loadStat("intelligence", cfg.stats.intelligence);
    loadStat("faith", cfg.stats.faith);
    loadStat("arcane", cfg.stats.arcane);

    auto spawn = ExtractObject(json, "spawn");
    if (auto b = JsonBool(spawn, "enabled")) cfg.spawn_enabled = *b;
    if (auto aob = JsonString(spawn, "spawn_fn_aob")) cfg.spawn_fn_aob = *aob;
    // spawn_radius may be float; accept int fallback
    if (auto r = JsonInt(spawn, "spawn_radius")) cfg.spawn_radius = static_cast<float>(*r);

    auto enemies = ExtractObject(spawn, "enemies");
    for (const char* key : {"dogs", "rats", "hawks"}) {
        auto obj = ExtractObject(enemies, key);
        if (obj.empty()) continue;
        EnemyDef def;
        if (auto m = JsonString(obj, "model")) def.model = *m;
        if (auto n = JsonString(obj, "name")) def.name = *n;
        if (auto id = JsonInt(obj, "npc_param_id")) def.npc_param_id = *id;
        if (auto id = JsonInt(obj, "think_param_id")) def.think_param_id = *id;
        cfg.enemies.emplace(key, std::move(def));
    }

    // Sensible built-in AOBs if file omitted them
    if (cfg.worldchr_aob.empty())
        cfg.worldchr_aob = "48 8B 05 ?? ?? ?? ?? 48 85 C0 74 0F 48 39 88";
    if (cfg.gamedata_aob.empty())
        cfg.gamedata_aob = "48 8B 05 ?? ?? ?? ?? 48 85 C0 74 08 48 8B 48 08";

    out = std::move(cfg);
    log::Info("Loaded config from " + path);
    return true;
}

}  // namespace chaos
