#pragma once

#include "util/config.hpp"
#include "game/targets.hpp"

#include <string>

namespace chaos::player {

bool HealHp(const Config& cfg, targets::Mode mode = targets::Mode::All);
bool RestoreFp(const Config& cfg, targets::Mode mode = targets::Mode::All);
bool LoseRunesPercent(const Config& cfg, int percent, targets::Mode mode = targets::Mode::All);
bool ApplyStatDelta(const Config& cfg, const std::string& stat, int delta,
                    targets::Mode mode = targets::Mode::All);
bool GiveRandomWeapon(const Config& cfg, targets::Mode mode = targets::Mode::All);
bool UnequipRandomGear(const Config& cfg, targets::Mode mode = targets::Mode::All);

// Vanilla-style rewards when a chaos-spawned boss dies (remembrance / great rune / set).
bool HasBossDrops(const std::string& enemy_key);
int GiveBossKillItems(const std::string& enemy_key);  // returns NG rune amount, or -1
int ReadRunes(const Config& cfg);                     // -1 if unread
bool AddRunes(const Config& cfg, int amount);
int ReadSoulLevel(const Config& cfg);                 // 0 if unread
int ReadLocalMaxHp(const Config& cfg);                // 0 if unread

}  // namespace chaos::player
