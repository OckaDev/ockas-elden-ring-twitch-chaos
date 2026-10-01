# Ocka's Twitch Viewer Interactable Chaos Mod For Elden Ring

**Version 1.1.0** — by Ocka

Let Twitch chat mess with your Elden Ring run. Channel Point rewards and subscriptions fire chaos effects in-game: heals, stat swings, random weapons, crabs, bosses, and more. Built for **Seamless Co-op** (offline). Not for EAC online.

> Short name: **Ocka's Elden Chaos**

## What chat can do

| Viewer action | In-game |
|---|---|
| Channel Point rewards you set up | Heal, restore FP, steal 10% runes, ±5 to a stat, give a random weapon, unequip a piece of gear, random Ash of War, swap left/right hand, drain or refill flasks, no rolling for 5 s, inverted movement for 10 s, spawn dogs / rats / hawks / crabs / lobster / runebear / Tree Sentinel |
| New sub / resub / single gifted sub | One random main boss at its normal in-game stats |
| Gift bomb of X subs | X random bosses |

Spawned enemies use their **normal in-game HP and damage**, and they spawn at **full HP**. When killed they give their runes and item drops (bosses also give remembrances / unique loot) **to every player running the DLL**, not only the last hit. Resting at a grace despawns them on every PC. Dying keeps the fight.

With friends running the same DLL, everyone gets their own copy of each spawn with **shared HP and posture**. Damage from any player counts, you can stance-break together, and everyone is paid out when it dies.

Boss pool: Margit, Godrick, Red Wolf of Radagon, Rennala, Godfrey (golden shade), Sir Gideon, Godskin Duo, Maliketh, Malenia, Fire Giant, Godfrey, Radagon, Elden Beast.

## How it fits together

```
Twitch  →  Streamer.bot  →  UDP 127.0.0.1:22666  →  EldenChaos.dll (in Elden Ring)
                 │
                 ├── Speaker.bot (optional TTS)
                 └── OBS overlay (optional banner)
```

Only the streamer's PC needs Streamer.bot. Friends in Seamless Co-op only need the DLL if you want them to get the spawns, player effects, and kill loot too.

## Setup

Full walkthrough: **[docs/INSTALL.md](docs/INSTALL.md)**

Streamer.bot paste-ins: **[streamerbot/SETUP.md](streamerbot/SETUP.md)**

1. Grab `EldenChaos.dll` from a [GitHub Release](https://github.com/OckaDev/ockas-elden-ring-twitch-chaos/releases), or build it (Visual Studio 2022 + CMake).
2. Drop the DLL + `config/` into Elden Ring `Game\mods\` ([Elden Mod Loader](https://www.nexusmods.com/eldenring/mods/117)) **or** load it beside Seamless via Mod Engine 2.
3. In Streamer.bot, paste `streamerbot/ChaosReward.cs` and `streamerbot/ChaosSub.cs`, then hook your Channel Point rewards and sub events.
4. Optional: OBS Browser Source → `overlay/index.html`, Speaker.bot for TTS.
5. Launch Elden Ring **offline / Seamless**. Never with Easy Anti-Cheat online.

Test without going live:

```powershell
python tools/send_event.py heal
python tools/send_event.py boss
python tools/send_event.py spawn lobster
python tools/send_event.py ash
python tools/send_event.py flasks drain
```

## Files you copy after updating

| What | File | Where it goes |
|---|---|---|
| In-game mod | `EldenChaos.dll` + `config/` | `ELDEN RING\Game\mods\` (game closed) |
| Channel points | `streamerbot/ChaosReward.cs` | Streamer.bot action **Elden Chaos Reward** — replace the C#, Compile |
| Subs | `streamerbot/ChaosSub.cs` | Streamer.bot action **Elden Chaos Sub** — replace the C#, Compile |
| Overlay | `overlay/index.html` | OBS Browser Source local file |

Friends need the new DLL too (same `secret` in `chaos_net.ini`). They do not need Streamer.bot or the overlay.

## Requirements

- Elden Ring (Steam)
- [Seamless Co-op](https://www.nexusmods.com/eldenring/mods/510)
- [Elden Mod Loader](https://www.nexusmods.com/eldenring/mods/117) **or** [Mod Engine 2](https://github.com/soulsmods/ModEngine2)
- [Streamer.bot](https://streamer.bot/)
- [Speaker.bot](https://speaker.bot/) (optional, TTS)
- Visual Studio 2022 (Desktop C++) + CMake 3.20+ to build the DLL

## Safety

- **Seamless Co-op / offline only.** Do not use this on the official online servers.
- Not affiliated with FromSoftware, Bandai Namco, or Twitch.
- Game patches can break memory signatures. If heals or spawns stop working, update `mod/config/signatures.json`.
- Never publish your `chaos_net.ini`. It holds a private secret and may contain an address.

## Versions

See [CHANGELOG.md](CHANGELOG.md). New versions will be tagged on GitHub Releases.

## License

[MIT](LICENSE) © Ocka
