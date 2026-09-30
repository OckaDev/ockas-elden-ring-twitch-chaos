"""Generate weapon_ids.hpp from Paramdex EquipParamWeapon names.

Usage:
    python tools/gen_weapon_ids.py path/to/EquipParamWeapon.txt
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

OUT = Path(__file__).resolve().parents[1] / "mod" / "include" / "game" / "weapon_ids.hpp"


def esc(name: str) -> str:
    return name.replace("\\", "\\\\").replace('"', '\\"')


def main() -> int:
    if len(sys.argv) < 2:
        print("usage: python tools/gen_weapon_ids.py EquipParamWeapon.txt")
        return 2
    src = Path(sys.argv[1])
    weapons: list[tuple[int, str]] = []
    for line in src.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line:
            continue
        m = re.match(r"^(\d+)\s+(.*)$", line)
        if not m:
            continue
        wid = int(m.group(1))
        name = m.group(2).strip()
        if wid < 1_000_000:
            continue
        if wid % 10000 != 0:
            continue
        if "[NPC]" in name:
            continue
        if 30_000_000 <= wid < 33_000_000:
            continue
        if 50_000_000 <= wid < 60_000_000:
            continue
        weapons.append((wid, name))

    lines = [
        "#pragma once",
        "",
        "// Auto-generated from soulsmods Paramdex EquipParamWeapon.txt",
        "// Unupgraded unique armaments only (no affinities, ammo, NPC fakes, or shields).",
        "",
        "namespace chaos::player {",
        "",
        "struct WeaponPick {",
        "    unsigned id;",
        "    const char* name;",
        "};",
        "",
        "constexpr WeaponPick kRandomWeapons[] = {",
    ]
    for wid, name in weapons:
        lines.append(f'    {{{wid}, "{esc(name)}"}},')
    lines += [
        "};",
        "",
        "}  // namespace chaos::player",
        "",
    ]
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text("\n".join(lines), encoding="utf-8")
    print(f"wrote {len(weapons)} weapons to {OUT}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
