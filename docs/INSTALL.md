# Install Ocka's Elden Chaos

This is the full setup for running **Ocka's Twitch Viewer Interactable Chaos Mod For Elden Ring** on your own PC.

You need: Elden Ring, Seamless Co-op, the chaos DLL, and Streamer.bot. Speaker.bot and the OBS overlay are optional.

---

## 1. Elden Ring + Seamless Co-op

1. Install Elden Ring on Steam.
2. Install [Seamless Co-op](https://www.nexusmods.com/eldenring/mods/510) as usual.
3. Always launch through Seamless / your mod launcher — **never** with Easy Anti-Cheat online.

---

## 2. Build the DLL

Requirements: [Visual Studio 2022](https://visualstudio.microsoft.com/) with **Desktop development with C++**, and [CMake](https://cmake.org/download/) 3.20+.

```powershell
cd mod
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

Or from `mod/`:

```powershell
powershell -ExecutionPolicy Bypass -File .\build.ps1
```

Output (one of these, depending on generator):

- `mod/build/Release/EldenChaos.dll`
- `mod/build/EldenChaos.dll`

Also copy `mod/config/signatures.json` next to the DLL (the build copies it into `config/` beside the DLL).

---

## 3. Load the DLL in-game

Pick **one** loader. Elden Mod Loader is the simpler path.

### Option A — Elden Mod Loader

1. Install [Elden Mod Loader](https://www.nexusmods.com/eldenring/mods/117).
2. Copy into `ELDEN RING\Game\mods\`:
   - `EldenChaos.dll`
   - `config\signatures.json` (keep the `config` folder)
3. You can start from `mod/mod_loader_config.ini` if you want a load-order example. It should list `EldenChaos.dll`.
4. Launch via Seamless as you normally do with other mods.

After a successful load you should get `elden_chaos.log` next to the DLL (or under `Game\mods\`).

### Option B — Mod Engine 2

Official Seamless + extra DLLs: https://ersc-docs.github.io/seamless-modding/

1. Put Mod Engine 2 in the Elden Ring `Game` folder.
2. Put Seamless's `SeamlessCoop` folder inside that ME2 folder.
3. Create `dllMods/` and copy `EldenChaos.dll` plus the `config` folder into it.
4. Merge the `external_dlls` list from `mod/config_eldenring.toml.example` into your `config_eldenring.toml`:

```toml
[modengine]
debug = false
external_dlls = [
  "SeamlessCoop/ersc.dll",
  "dllMods/EldenChaos.dll"
]
```

5. Launch with `launchmod_eldenring.bat`.

---

## 4. Streamer.bot

Follow **[streamerbot/SETUP.md](../streamerbot/SETUP.md)** for the exact C# paste, triggers, and Channel Point argument names.

Summary:

1. Enable **Servers / Clients → Websocket Server** (port `8080`) if you want the overlay.
2. Action `Elden Chaos Reward` → paste `ChaosReward.cs` → hook each Channel Point reward with `chaosEffect=...`
3. Action `Elden Chaos Sub` → paste `ChaosSub.cs` → hook Sub, ReSub, Gift Subscription, Gift Bomb.
4. Optional: Speaker.bot + `TtsVoiceAlias` in the C# (must match a Speaker.bot voice alias).

The DLL listens on **UDP `127.0.0.1:22666`**. Streamer.bot must run on the **same PC** as the game.

---

## 5. OBS overlay (optional)

1. Add a **Browser Source**.
2. Local file: `overlay/index.html`
3. Width ~1280, height ~400, **transparent** background.
4. Preview in a browser: `overlay/index.html?preview=1`

Streamer.bot broadcasts JSON to the overlay over its websocket. The C# already does this.

---

## 6. Test without going live

With the game sitting at a grace (DLL loaded):

```powershell
python tools/send_event.py heal
python tools/send_event.py spawn lobster
python tools/send_event.py boss
python tools/send_event.py boss --count 3
```

Without the game, confirm Streamer.bot is sending UDP:

```powershell
python tools/mock_game_listener.py
```

Then **Test Action** / **Test Trigger** in Streamer.bot. Close `mock_game_listener.py` before starting Elden Ring — both bind the same port.

---

## 7. Seamless friends (optional)

Friends do **not** need Streamer.bot. They only need the DLL if you want them to see spawned enemies and have HP/stat effects on their own character.

1. First time the host launches, the DLL writes `mods/chaos_net.ini` with an empty `host=` and a random `secret`.
2. Send friends **privately**: the secret, port `22667`, and a reachable address (LAN or a VPN you both use). Do not put that file on GitHub.
3. Friend copies `EldenChaos.dll` + `config/` into their `Game\mods\` and makes `chaos_net.ini` from `mod/chaos_net.ini.example`, filling in `host=` and the same secret.
4. Check logs: `Net: friend connected` on the host, `Net: connected to host` on the friend.

TCP **22667** must be reachable host ← friend (LAN, VPN, or a router forward you set up yourself).

---

## 8. After an Elden Ring patch

If heals, stats, or spawns stop working, signatures moved. Update `mod/config/signatures.json` (and recopy it next to the DLL). Offset-only changes do not require a rebuild.

Replace the DLL only while the game is **closed**.

---

## Safety

- Seamless / offline only
- Not affiliated with FromSoftware, Bandai Namco, or Twitch
- Keep `chaos_net.ini` private
