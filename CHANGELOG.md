# Changelog

## 1.1.0 — 2026-10-01

- Spawned enemies no longer try to share position or attacks. Each PC gets its own copy with **shared HP and posture**.
- When a spawned enemy dies, **every connected player** gets the runes and item drops, not only the last hit.
- Resting at a grace despawns those enemies on every PC. Dying keeps the fight and restores it at the shared HP.
- Enemies spawn at full HP for the area you are in.
- New Channel Point effects (hit everyone): random Ash of War, swap hands, drain or refill flasks, no rolling for 5s, inverted movement for 10s.
- Fire Giant is back in the random sub boss pool.
- Friend link talks HP / posture / kills only. Keep `chaos_net.ini` private.

## 1.0.0 — 2026-09-30

First public release of **Ocka's Twitch Viewer Interactable Chaos Mod For Elden Ring**.

- Channel Point rewards: heal, FP, runes, stats, weapons, unequip, and enemy spawns
- Twitch subs / resubs / gifted subs spawn random bosses; gift bombs spawn one boss per gift (capped)
- Optional Seamless Co-op friend link so party members see the same spawns
- Streamer.bot overlay + optional Speaker.bot TTS
