# Install Ocka's Elden Chaos

This is the full setup for **Ocka's Twitch Viewer Interactable Chaos Mod For Elden Ring**.

You need: Elden Ring, Seamless Co-op, the chaos DLL, and Streamer.bot. Speaker.bot and the OBS overlay are optional.

Nothing in this guide should include a real IP address, Windows username, or Twitch login. Keep `chaos_net.ini` on your own machines only.

---

## 1. Elden Ring + Seamless Co-op

1. Install Elden Ring on Steam.
2. Install [Seamless Co-op](https://www.nexusmods.com/eldenring/mods/510) the usual way.
3. Always launch through Seamless / your mod launcher — **never** with Easy Anti-Cheat online.

Confirm Seamless works (load a save, rest at a grace) before adding this mod.

---

## 2. Get the DLL

### Option A — GitHub Release (easiest)

1. Open the repo’s [Releases](https://github.com/OckaDev/ockas-elden-ring-twitch-chaos/releases) page.
2. Download the latest `EldenChaos.dll` (and the `config` folder if the zip includes it).

### Option B — Build it yourself

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

Output is one of:

- `mod/build/Release/EldenChaos.dll`
- `mod/build/EldenChaos.dll`

The build also copies `mod/config/signatures.json` next to the DLL.

---

## 3. Load the DLL in-game

Pick **one** loader. Elden Mod Loader is the simpler path.

**Replace the DLL only while Elden Ring is fully closed.**

### Option A — Elden Mod Loader

1. Install [Elden Mod Loader](https://www.nexusmods.com/eldenring/mods/117).
2. Copy into `ELDEN RING\Game\mods\`:
   - `EldenChaos.dll`
   - `config\signatures.json` (keep the `config` folder)
3. You can start from `mod/mod_loader_config.ini` if you want a load-order example. It should list `EldenChaos.dll`.
4. Launch via Seamless as you normally do with other mods.

After a successful load you should get `elden_chaos.log` next to the DLL (usually `Game\mods\elden_chaos.log`).

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

## 4. Streamer.bot (streamer's PC only)

Streamer.bot must run on the **same PC** as Elden Ring. The DLL listens on **UDP `127.0.0.1:22666`**.

Follow **[streamerbot/SETUP.md](../streamerbot/SETUP.md)** for the exact C# paste, triggers, and argument names. Short version:

1. Enable **Servers / Clients → Websocket Server** (port `8080`) if you want the overlay.
2. Action `Elden Chaos Reward` → paste **the whole file** `streamerbot/ChaosReward.cs` → Compile → hook each Channel Point reward with `chaosEffect=...`
3. Action `Elden Chaos Sub` → paste **the whole file** `streamerbot/ChaosSub.cs` → Compile → hook Sub, ReSub, Gift Subscription, Gift Bomb.
4. Optional: Speaker.bot. Set `TtsVoiceAlias` in the C# to a voice alias that exists in Speaker.bot.

When you update the mod, **re-paste both C# files** and Compile again. New effects (Ash of War, flasks, no roll, invert) will not exist until you do.

### Suggested Channel Point rewards

Create these on Twitch, then point each one at `Elden Chaos Reward` with `chaosEffect` set to the value in the middle column:

| Reward title (example) | `chaosEffect` | Who it hits |
|---|---|---|
| Heal HP | `heal_hp` | everyone |
| Restore FP | `restore_fp` | everyone |
| Steal Runes | `lose_runes` | everyone |
| Random Weapon | `give_weapon` | everyone |
| Unequip Gear | `unequip` | everyone |
| Random Ash of War | `ash_of_war` | everyone |
| Swap Hands | `swap_hands` | everyone |
| Drain or Refill Flasks | `flasks` | everyone (one coin flip for the whole party) |
| No Rolling | `no_roll` | everyone, 5 seconds |
| Invert Movement | `invert_move` | everyone, 10 seconds |
| Vigor / Mind / … | `vigor` … `arcane` | everyone, +5 or −5 |
| Spawn Dogs | `dogs` | random player, ×3 |
| Spawn Rats | `rats` | random player, ×3 |
| Spawn Hawks | `hawks` | random player, ×3 |
| Spawn Crabs | `crabs` | random player, ×3 |
| Spawn Lobster | `lobster` | random player |
| Spawn Runebear | `bear` | random player |
| Spawn Tree Sentinel | `tree_sentinel` | random player |

Do **not** make a Channel Point for `boss`. Subs already spawn bosses via `ChaosSub.cs`.

If you skip `chaosEffect`, the script tries to guess from the Twitch reward title (`Heal`, `Ash`, `Flask`, `Lobster`, …). Setting the argument is more reliable.

---

## 5. OBS overlay (optional)

The overlay is a banner that shows who redeemed what. It talks to Streamer.bot’s websocket. It does **not** talk to the game.

1. In Streamer.bot, confirm **Servers / Clients → Websocket Server** is enabled on port `8080`.
2. In OBS, add a **Browser Source**.
3. Local file: `overlay/index.html` from this repo (or a copy on disk).
4. Width ~1280, height ~400, check **Shutdown source when not visible** off, **Refresh browser when scene becomes active** optional.
5. Set the source to a **transparent** background.
6. Preview in a normal browser: open `overlay/index.html?preview=1` — you should see a fake “Preview spawned 3 dogs” banner.

You only need to replace `index.html` in OBS if the overlay file itself changed. Re-pasting Streamer.bot C# does not require a new HTML file unless the overlay protocol changed.

---

## 6. Test without going live

With Elden Ring sitting at a grace (DLL loaded, `elden_chaos.log` growing):

```powershell
python tools/send_event.py heal
python tools/send_event.py spawn lobster
python tools/send_event.py ash
python tools/send_event.py flasks drain
python tools/send_event.py flasks refill
python tools/send_event.py no_roll
python tools/send_event.py invert
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

Friends do **not** need Streamer.bot, Speaker.bot, or the overlay. They only need the DLL if you want them to:

- see spawned enemies on their own PC
- have HP / stat / flask / ash effects on their character
- get **the same runes and item drops** when a spawned enemy dies

### Host (streamer)

1. Launch Elden Ring with the DLL once. It writes `Game\mods\chaos_net.ini` with an empty `host=` line and a random `secret`.
2. Open that file in a text editor. Copy the `secret=` value.
3. Tell each friend, **privately** (DMs, not the GitHub repo):
   - the secret
   - port `22667`
   - a reachable address for your PC (LAN IP, or a VPN address you both use)
4. Do not commit `chaos_net.ini` and do not put those values in a public gist or Discord channel.

### Friend

1. Copy the **same version** of `EldenChaos.dll` and the `config` folder into their `Game\mods\`.
2. Copy `mod/chaos_net.ini.example` to `Game\mods\chaos_net.ini`.
3. Set:
   - `host=` to the address the host gave you (no `http://`, no port on this line)
   - `port=22667`
   - `secret=` to the exact same secret
4. Launch through Seamless and join the host as usual.

### Check it worked

- Host log: `Net: friend connected`
- Friend log: `Net: connected to host`

TCP **22667** must be reachable **from the friend to the host** (same LAN, a VPN, or a port forward you set up yourself). UDP `22666` stays on localhost; friends never send Channel Point packets.

When a spawned enemy dies, every connected PC pays out runes and items to its own player. Resting at a grace despawns the copies on every PC.

---

## 8. After an Elden Ring patch

If heals, stats, or spawns stop working, signatures moved. Update `mod/config/signatures.json` and recopy it next to the DLL. Offset-only changes do not require a rebuild.

Always close the game before replacing `EldenChaos.dll`.

---

## 9. Troubleshooting

| Symptom | What to check |
|---|---|
| Nothing happens on a redeem | Streamer.bot on the game PC? UDP 22666? `elden_chaos.log` getting a `UDP payload` line? |
| Overlay blank | Websocket Server enabled on 8080? OBS pointing at `overlay/index.html`? Try `?preview=1` in a browser. |
| Friend does not see spawns | Same DLL version, same secret, `host=` set, `Net: friend connected` in the host log. |
| Enemy HP not shared | Both PCs show `Shared HP tracking` in the log after a spawn. |
| Rest does not despawn | Look for `Rest detected` in the log. Friend should get `Net rest`. |
| No runes / items | Look for `Kill reward` / `Kill loot from peer` on **each** PC. |
| Ash of War / flasks do nothing | Re-paste `ChaosReward.cs`. Check the log for `Ash of War:` or `Flasks`. |
| Game crash on inject | Close the game fully, copy the DLL again, launch Seamless (not EAC). |

---

## Safety

- Seamless / offline only
- Not affiliated with FromSoftware, Bandai Namco, or Twitch
- Keep `chaos_net.ini` private
