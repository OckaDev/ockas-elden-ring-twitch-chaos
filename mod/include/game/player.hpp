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

// Our own player only (every PC runs the same command for itself).
bool RandomizeAshOfWar(const Config& cfg);
bool SwapHands(const Config& cfg);
bool SetFlasks(const Config& cfg, bool fill);

bool GiveItem(unsigned item_id, unsigned qty);
// Fixed loot a chaos-spawned enemy hands out on death (remembrances, unique
// drops). Returns the table rune amount, used when the NpcParam has none.
int GiveKillLoot(const std::string& enemy_key);
int ReadRunes(const Config& cfg);                     // -1 if unread
bool AddRunes(const Config& cfg, int amount);
int ReadSoulLevel(const Config& cfg);                 // 0 if unread
int ReadLocalMaxHp(const Config& cfg);                // 0 if unread

}  // namespace chaos::player
