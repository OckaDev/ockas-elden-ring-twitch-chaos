#pragma once

#include "util/config.hpp"
#include "game/targets.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace chaos::spawner {

bool SpawnNearTargets(const Config& cfg, const std::string& enemy_key, int count, targets::Mode mode);

std::string ResolveKey(const std::string& enemy_key);
bool PickParticipantPosition(const Config& cfg, int participant, float& x, float& y, float& z);
bool SpawnAtPosition(const Config& cfg, const std::string& enemy_key, int count,
                     float x, float y, float z);

// Host: unique ids so both PCs can puppet the same spawn over the friend link.
bool ReserveRuntimeHandles(const Config& cfg, int count, std::vector<uint64_t>& out_handles);
// Every PC creates the requested enemy. Host HP/position is copied to friends.
bool SpawnCreatedWithHandles(const Config& cfg, const std::string& enemy_key,
                             const std::vector<uint64_t>& handles, float x, float y, float z);

// Host-authority copy of our spawned enemies (Seamless does not replicate debug creates).
void RegisterSynced(uint64_t handle, uintptr_t chr);
void ApplyEntState(uint64_t handle, uint32_t seq, int hp, int mx, float x, float y, float z, bool dead,
                   int anim_id, float anim_t, bool have_rot, float ang1, float ang2);
void ApplyEntDamage(uint64_t handle, int hp);

bool SpawnNearPlayer(const Config& cfg, const std::string& enemy_key, int count);

}  // namespace chaos::spawner
