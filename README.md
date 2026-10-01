# Ocka's Twitch Viewer Interactable Chaos Mod For Elden Ring

**Version 1.0.0** — by Ocka

Let Twitch chat mess with your Elden Ring run. Channel Point rewards and subscriptions fire chaos effects in-game: heals, stat swings, random weapons, crabs, bosses, and more. Built for **Seamless Co-op** (offline). Not for EAC online.

> Short name: **Ocka's Elden Chaos**

## What chat can do

| Viewer action | In-game |
|---|---|
| Channel Point rewards you set up | Heal, restore FP, steal 10% runes, ±5 to a stat, give a random weapon, unequip a piece of gear, random Ash of War, swap left/right hand, drain or refill flasks, no rolling for 5 s, inverted movement for 10 s, spawn dogs / rats / hawks / crabs / lobster / runebear / Tree Sentinel |
| New sub / resub / single gifted sub | One random main boss at its normal in-game stats |
| Gift bomb of X subs | X random bosses |

Spawned enemies use their normal in-game HP and damage. When killed they give their runes and item drops, and bosses also give their remembrance / unique loot. Resting at a grace despawns them.

With friends running the DLL, everyone gets their own copy of each spawn with **shared HP and posture**, so damage from any player counts and you can stance-break it together.

Boss pool: Margit, Godrick, Red Wolf of Radagon, Rennala, Godfrey (golden shade), Sir Gideon, Godskin Duo, Maliketh, Malenia, Fire Giant, Godfrey, Radagon, Elden Beast.

## How it fits together

```
Twitch  →  Streamer.bot  →  UDP 127.0.0.1:22666  →  EldenChaos.dll (in Elden Ring)
                 │
                 ├── Speaker.bot (optional TTS)
                 └── OBS overlay (optional banner)
```

Only the streamer's PC needs Streamer.bot. Friends in Seamless Co-op only need the DLL if you want them to get the spawns and player effects too.

## Setup (other streamers)

Full steps: **[docs/INSTALL.md](docs/INSTALL.md)**

Streamer.bot paste-ins: **[streamerbot/SETUP.md](streamerbot/SETUP.md)**

1. Build `EldenChaos.dll` (Visual Studio 2022 + CMake) or use a release DLL when one is attached to a GitHub Release.
2. Drop the DLL + `config/` into Elden Ring `Game\mods\` ([Elden Mod Loader](https://www.nexusmods.com/eldenring/mods/117)) **or** load it beside Seamless via Mod Engine 2.
3. In Streamer.bot, paste `streamerbot/ChaosReward.cs` and `streamerbot/ChaosSub.cs`, then hook your Channel Point rewards and sub events.
4. Optional: OBS Browser Source → `overlay/index.html`, Speaker.bot for TTS.
5. Launch Elden Ring **offline / Seamless**. Never with Easy Anti-Cheat online.

Test without going live:

```powershell
python tools/send_event.py heal
python tools/send_event.py boss
python tools/send_event.py spawn lobster
```

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
