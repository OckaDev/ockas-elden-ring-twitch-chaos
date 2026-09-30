#pragma once

#include <string>

namespace chaos {

// From Streamer.bot (UDP) on the host PC.
void HandleCommandJson(const std::string& json);
// From the host over the friend link: always applies to this PC's own player.
void HandleRemoteCommandJson(const std::string& json);

}  // namespace chaos
