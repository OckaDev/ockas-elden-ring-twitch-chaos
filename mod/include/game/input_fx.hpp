#pragma once

namespace chaos::inputfx {

// Dodge roll / backstep / quickstep blocked for `ms` on this PC's player.
void BlockRoll(int ms);
// Left stick and WASD reversed for `ms` on this PC.
void InvertMovement(int ms);

}  // namespace chaos::inputfx
