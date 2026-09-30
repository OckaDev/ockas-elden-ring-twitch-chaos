#pragma once

#include "util/config.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace chaos::targets {

enum class Mode {
    All,
    Random,
    Local,  // only this machine's own player (multiplayer link: every PC runs the mod)
};

struct PlayerRef {
    uintptr_t chr = 0;       // ChrIns / PlayerIns
    uintptr_t char_data = 0; // CSChrData (HP/FP block) if resolved
    int slot = -1;           // NetPlayers index, -1 = local-only
    bool is_local = false;
};

Mode ParseMode(const std::string& s);
std::vector<PlayerRef> CollectPlayers(const Config& cfg);
std::vector<PlayerRef> SelectTargets(const Config& cfg, Mode mode);
std::string DescribeTargets(const std::vector<PlayerRef>& targets);

}  // namespace chaos::targets
