#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <optional>
#include <unordered_map>

namespace chaos {

struct StatOffsets {
    uintptr_t vigor = 0x40;
    uintptr_t mind = 0x44;
    uintptr_t endurance = 0x48;
    uintptr_t strength = 0x4C;
    uintptr_t dexterity = 0x50;
    uintptr_t intelligence = 0x54;
    uintptr_t faith = 0x58;
    uintptr_t arcane = 0x60;
};

struct EnemyDef {
    std::string model;
    int npc_param_id = 0;
    int think_param_id = 0;
    std::string name;
};

struct Config {
    int udp_port = 22666;

    std::string worldchr_aob;
    int worldchr_rip_offset_at = 3;
    int worldchr_rip_size = 7;

    std::string gamedata_aob;
    int gamedata_rip_offset_at = 3;
    int gamedata_rip_size = 7;

    uintptr_t worldchr_player_offset = 0x10EF8;
    uintptr_t chr_ins_module_base = 0x190;
    uintptr_t hp_current = 0x138;
    uintptr_t hp_max = 0x144;
    uintptr_t gamedata_player_game_data = 0x08;
    StatOffsets stats{0x3C, 0x40, 0x44, 0x48, 0x4C, 0x50, 0x54, 0x58};

    bool spawn_enabled = true;
    std::string spawn_fn_aob;
    float spawn_radius = 2.5f;
    std::unordered_map<std::string, EnemyDef> enemies;
};

// Loads config from <dll_dir>/config/signatures.json. Safe to call often.
bool LoadConfig(Config& out);
std::string DllDirectory();

}  // namespace chaos
