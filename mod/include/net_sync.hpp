#pragma once

#include <string>

namespace chaos::net {

// Reads <dll_dir>/chaos_net.ini and starts either:
//   - host   : TCP listener; forwards Streamer.bot commands to connected friends
//   - client : connects out to the host and executes the commands it receives
void Start();
void Stop();

bool IsClient();
int ClientCount();

// Host only.
void Broadcast(const std::string& json);
bool SendToClient(int index, const std::string& json);

// Friend PC: send a line back to the host (damage reports).
bool SendToHost(const std::string& json);

}  // namespace chaos::net
