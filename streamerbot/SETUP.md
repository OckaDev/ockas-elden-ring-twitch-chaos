# Streamer.bot setup (channel points + sub)

## 1. Enable servers

1. **Servers / Clients → Websocket Server** → Enable (port `8080`)
2. Optional: **Integrations → Speaker.bot** for TTS

## 2. Create the reward action

1. **Actions → Add** → name it `Elden Chaos Reward`
2. Add **Core → C# → Execute C# Code**
3. Paste `ChaosReward.cs` → **Compile** → OK
4. Optional: set `TtsVoiceAlias` to your Speaker.bot alias (`Zira`, etc.)

### Per Twitch reward

For each Channel Point reward:

1. Twitch: create the reward with a clear title (e.g. `Heal HP`, `Spawn 3 Dogs`, `Spawn Lobster`)
2. Streamer.bot: **Triggers → Twitch → Channel Reward Redemption** → pick that reward
3. Link to action `Elden Chaos Reward`
4. On the action (or a Set Argument sub-action **before** the C#), set:

| Argument | Example |
|---|---|
| `chaosEffect` | `heal_hp` |

If `chaosEffect` is missing, the script tries to map the Twitch **reward title** automatically.

## 2b. Subs and gifted subs (random bosses)

Use a **separate** action so gift bombs do not double-fire with channel points.

1. **Actions → Add** → name it `Elden Chaos Sub`
2. Add **Core → C# → Execute C# Code**
3. Paste **`ChaosSub.cs`** → **Compile** → OK
4. If channel-point TTS works but subs stay silent, add the **same Speaker.bot Speak sub-action** you use on `Elden Chaos Reward`, with text `%chaosMessage%`. Do this **after** the C# step.
5. Add these triggers on that same action:

| Trigger | Notes |
|---|---|
| **Twitch → Sub** | New subscriber → 1 random boss |
| **Twitch → ReSub** | Resub → 1 random boss |
| **Twitch → Gift Subscription** | One gifted sub → 1 random boss. On this trigger, enable **Ignore Gift Subs from Bombs** |
| **Twitch → Gift Bomb** | Someone gifts **X** subs → **X** random bosses (capped at 12). On this trigger, enable **Tier 1, 2, and 3** and **Public + Anonymous** or Test Trigger will not fire. |

Do **not** also set `chaosEffect=boss` on `Elden Chaos Reward` for these Twitch sub triggers, or you will spawn twice.

Test without going live:

```powershell
python tools/send_event.py boss
python tools/send_event.py boss --count 3
```

In Streamer.bot you can also **Test Trigger** on Gift Bomb if your version has it.

## 3. Effect catalog

### All players (`target=all`)

| `chaosEffect` | What it does |
|---|---|
| `heal_hp` | Full HP |
| `restore_fp` | Full FP |
| `lose_runes` | −10% held runes |
| `give_weapon` | Adds a random vanilla weapon to inventory |
| `unequip` | Unequips a random weapon, armor piece, or talisman |
| `vigor` … `arcane` | That stat **+5 or −5** (rolled) |

### Random player (`target=random`)

Works with 1–4 Seamless players (alone = always you).

| `chaosEffect` | Spawn |
|---|---|
| `lobster` | 1 lobster |
| `crabs` | 3 crabs |
| `bear` | 1 runebear |
| `tree_sentinel` | 1 Tree Sentinel |
| `dogs` | 3 dogs/wolves |
| `rats` | 3 rats |
| `hawks` | 3 warhawks |
| `boss` | Random main boss — **use `ChaosSub.cs` for Twitch subs**, not channel points |

Boss pool: Margit, Godrick, Red Wolf, Rennala, Godfrey shade, Gideon, Godskin Duo, Maliketh, Fire Giant, Godfrey/Hoarah, Radagon, Elden Beast.

Spawns **create** the enemy near the player (Seamless-synced when friends also have the DLL). Dogs/rats/hawks can fall back to relocating an already-loaded copy if create fails.

## 4. Test commands (game running)

```powershell
python tools/send_event.py heal
python tools/send_event.py fp
python tools/send_event.py runes
python tools/send_event.py give_weapon
python tools/send_event.py unequip
python tools/send_event.py stat vigor 5
python tools/send_event.py spawn dogs
python tools/send_event.py spawn lobster
python tools/send_event.py spawn tree_sentinel
python tools/send_event.py boss
python tools/send_event.py boss --count 3
python tools/send_event.py spawn dogs --target all
```

## 4b. Seamless friends (optional)

Friends who join your Seamless session can install the same DLL so they see spawned enemies. They do not need Streamer.bot.

1. Host: first launch creates `mods/chaos_net.ini` (`host=` empty, random `secret`).
2. Share the **secret**, **port 22667**, and a reachable **LAN or VPN address** privately. Do not commit `chaos_net.ini`.
3. Friend: same `EldenChaos.dll` + `config/` folder, plus `chaos_net.ini` with `host=<host address>` and the same secret. See `mod/chaos_net.ini.example`.
4. When a friend is connected, `target=all` hits everyone; `target=random` picks the host or one friend. Each PC edits its own character.
5. Log lines: `Net: friend connected`, `Net: connected to host`.

TCP 22667 must be reachable from friend to host (LAN, VPN, or your own port forward).

## 5. Notes (Seamless)

- **HP / FP / spawn-at-player** use live player instances → work across the party from the PC running the DLL (usually host).
- **Runes / stats / equip** edit **local** `PlayerGameData` on that PC. With one DLL on the host, those hit the host character; each friend would need the mod for full “everyone’s sheet” edits.
- Old `ChaosDice.cs` still works for a single random dice reward if you want it.
