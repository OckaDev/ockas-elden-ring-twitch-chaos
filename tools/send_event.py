#!/usr/bin/env python3
"""Manually fire EldenChaos UDP events (no Streamer.bot needed)."""

from __future__ import annotations

import argparse
import json
import socket

PORT = 22666

STATS = [
    "vigor", "mind", "endurance", "strength",
    "dexterity", "intelligence", "faith", "arcane",
]

ENEMIES = [
    "dogs", "rats", "hawks", "lobster", "crabs", "bear", "tree_sentinel",
    "boss_random", "boss_margit", "boss_godrick", "boss_red_wolf", "boss_rennala",
    "boss_godfrey_shade", "boss_gideon", "boss_godskin", "boss_maliketh",
    "boss_malenia", "boss_fire_giant", "boss_godfrey", "boss_radagon", "boss_elden_beast",
]

EFFECTS = [
    "heal_hp", "restore_fp", "lose_runes", "give_weapon",
    "unequip", "stat", "spawn", "boss",
    "ash_of_war", "swap_hands", "flasks", "no_roll", "invert_move",
]


def send(payload: dict) -> None:
    data = json.dumps(payload).encode("utf-8")
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.sendto(data, ("127.0.0.1", PORT))
    print("sent:", json.dumps(payload))


def main() -> None:
    p = argparse.ArgumentParser(description="Send EldenChaos UDP test events")
    p.add_argument("--user", default="TestUser")
    p.add_argument("--target", choices=["all", "random"], default=None,
                   help="Override target (default depends on effect)")
    sub = p.add_subparsers(dest="cmd", required=True)

    sub.add_parser("heal")
    sub.add_parser("fp")
    sub.add_parser("runes")
    sub.add_parser("give_weapon")
    sub.add_parser("unequip")
    sub.add_parser("ash")
    sub.add_parser("swap")
    fl = sub.add_parser("flasks")
    fl.add_argument("mode", choices=["drain", "refill", "random"], nargs="?", default="random")
    nr = sub.add_parser("no_roll")
    nr.add_argument("--ms", type=int, default=5000)
    inv = sub.add_parser("invert")
    inv.add_argument("--ms", type=int, default=10000)

    st = sub.add_parser("stat")
    st.add_argument("stat", choices=STATS)
    st.add_argument("delta", type=int, nargs="?", default=None, help="+5/-5; omit to let DLL roll")

    sp = sub.add_parser("spawn")
    sp.add_argument("enemy", choices=ENEMIES)
    sp.add_argument("--count", type=int, default=None)

    boss_p = sub.add_parser("boss")
    boss_p.add_argument("--count", type=int, default=1)

    # Unified
    ef = sub.add_parser("effect")
    ef.add_argument("name", choices=EFFECTS)
    ef.add_argument("--stat", choices=STATS)
    ef.add_argument("--enemy", choices=ENEMIES)
    ef.add_argument("--count", type=int)
    ef.add_argument("--delta", type=int)

    args = p.parse_args()
    user = args.user

    def tgt(default: str) -> str:
        return args.target or default

    if args.cmd == "heal":
        send({"cmd": "effect", "effect": "heal_hp", "target": tgt("all"), "user": user,
              "message": f"{user} healed everyone"})
    elif args.cmd == "fp":
        send({"cmd": "effect", "effect": "restore_fp", "target": tgt("all"), "user": user,
              "message": f"{user} restored FP"})
    elif args.cmd == "runes":
        send({"cmd": "effect", "effect": "lose_runes", "target": tgt("all"), "percent": 10,
              "user": user, "message": f"{user} stole 10% runes"})
    elif args.cmd == "give_weapon":
        send({"cmd": "effect", "effect": "give_weapon", "target": tgt("all"), "user": user,
              "message": f"{user} gave a weapon"})
    elif args.cmd == "unequip":
        send({"cmd": "effect", "effect": "unequip", "target": tgt("all"), "user": user,
              "message": f"{user} unequipped gear"})
    elif args.cmd == "ash":
        send({"cmd": "effect", "effect": "ash_of_war", "target": tgt("all"), "user": user,
              "message": f"{user} randomized Ash of War"})
    elif args.cmd == "swap":
        send({"cmd": "effect", "effect": "swap_hands", "target": tgt("all"), "user": user,
              "message": f"{user} swapped hands"})
    elif args.cmd == "flasks":
        payload = {"cmd": "effect", "effect": "flasks", "target": tgt("all"), "user": user,
                   "message": f"{user} rolled the flasks"}
        if args.mode != "random":
            payload["mode"] = args.mode
        send(payload)
    elif args.cmd == "no_roll":
        send({"cmd": "effect", "effect": "no_roll", "ms": args.ms, "target": tgt("all"),
              "user": user, "message": f"{user} disabled rolling"})
    elif args.cmd == "invert":
        send({"cmd": "effect", "effect": "invert_move", "ms": args.ms, "target": tgt("all"),
              "user": user, "message": f"{user} inverted movement"})
    elif args.cmd == "stat":
        payload = {"cmd": "effect", "effect": "stat", "stat": args.stat, "target": tgt("all"),
                   "user": user, "message": f"{user} changed {args.stat}"}
        if args.delta is not None:
            payload["delta"] = args.delta
        send(payload)
    elif args.cmd == "spawn":
        defaults = {
            "dogs": 3, "rats": 3, "hawks": 3, "crabs": 3,
            "lobster": 1, "bear": 1, "tree_sentinel": 1,
        }
        count = args.count if args.count is not None else defaults.get(args.enemy, 1)
        send({"cmd": "effect", "effect": "spawn", "enemy": args.enemy, "count": count,
              "target": tgt("random"), "user": user,
              "message": f"{user} spawned {count} {args.enemy}"})
    elif args.cmd == "boss":
        n = args.count if args.count is not None else 1
        send({"cmd": "effect", "effect": "boss", "enemy": "boss_random", "count": n,
              "target": tgt("random"), "user": user,
              "message": f"{user} summoned {n} boss" + ("" if n == 1 else "es")})
    elif args.cmd == "effect":
        payload = {"cmd": "effect", "effect": args.name, "target": tgt("all"), "user": user,
                   "message": f"{user} triggered {args.name}"}
        if args.stat:
            payload["stat"] = args.stat
        if args.enemy:
            payload["enemy"] = args.enemy
        if args.count is not None:
            payload["count"] = args.count
        if args.delta is not None:
            payload["delta"] = args.delta
        send(payload)


if __name__ == "__main__":
    main()
