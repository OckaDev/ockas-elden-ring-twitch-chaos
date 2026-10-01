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

// Host: ids that name the same spawn on every PC.
bool ReserveRuntimeHandles(const Config& cfg, int count, std::vector<uint64_t>& out_handles);
// Every PC creates its own copy of the enemy at the same spot. Only HP and posture are shared.
bool SpawnCreatedWithHandles(const Config& cfg, const std::string& enemy_key,
                             const std::vector<uint64_t>& handles, float x, float y, float z);

// Host: a friend's copy lost this fraction of its max HP.
void OnRemoteDamage(uint64_t handle, float hp_frac);
// Friend: the host's shared HP fraction.
void OnSharedHp(uint64_t handle, float frac);
// Posture damage dealt on another PC's copy (fraction of max). origin 0 = host.
void OnRemotePosture(uint64_t handle, float posture_frac, uint32_t origin);
// Shared kill: grant this PC the runes and item drops (once per handle).
void OnRemoteKill(uint64_t handle, const std::string& key, int npc, int model);

// Grace rest unloads every debug spawn. tell_peer notifies the other PC.
void ClearSpawned(bool tell_peer);

bool SpawnNearPlayer(const Config& cfg, const std::string& enemy_key, int count);

uintptr_t LocalPlayerChr();

}  // namespace chaos::spawner
