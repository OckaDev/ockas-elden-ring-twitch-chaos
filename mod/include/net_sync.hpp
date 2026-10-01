#pragma once

#include <string>

namespace chaos::net {

// Reads <dll_dir>/chaos_net.ini and starts either:
//   - host   : TCP listener; forwards Streamer.bot commands to connected friends
//   - client : connects out to the host and executes the commands it receives
// local_test: a second game copy on this PC while the first one hosts. It joins
// 127.0.0.1 as a friend, so sync can be tested without a second machine.
void Start(bool local_test = false);
void Stop();

bool IsClient();
int ClientCount();

// Host only.
void Broadcast(const std::string& json);
bool SendToClient(int index, const std::string& json);

// Friend PC: send a line back to the host (damage reports).
bool SendToHost(const std::string& json);

}  // namespace chaos::net
