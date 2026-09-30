#include "game/targets.hpp"
#include "game/memory.hpp"
#include "util/log.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <optional>
#include <string>
#include <unordered_set>

namespace chaos::targets {
namespace {

std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::optional<uintptr_t> ResolveWorldChrMan(const Config& cfg) {
    auto hit = mem::FindAob(cfg.worldchr_aob);
    if (!hit) return std::nullopt;
    auto slot = mem::ResolveRip(*hit, cfg.worldchr_rip_offset_at, cfg.worldchr_rip_size);
    if (!slot) return std::nullopt;
    uintptr_t world = 0;
    if (!mem::ReadPtr(*slot, world) || !world) return std::nullopt;
    return world;
}

bool ResolveCharData(uintptr_t chr, const Config& cfg, uintptr_t& out) {
    uintptr_t modules = 0;
    if (!mem::ReadPtr(chr + cfg.chr_ins_module_base, modules) || !modules) return false;

    // Prefer fragile layout: modules -> * -> HP block
    uintptr_t inner = 0;
    if (mem::ReadPtr(modules, inner) && inner) {
        int hp = 0, mx = 0;
        if (mem::Read(inner + cfg.hp_current, hp) && mem::Read(inner + cfg.hp_max, mx)) {
            if (hp >= 0 && hp <= 500000 && mx >= 1 && mx <= 500000) {
                out = inner;
                return true;
            }
        }
    }

    // Direct module HP
    int hp = 0, mx = 0;
    if (mem::Read(modules + cfg.hp_current, hp) && mem::Read(modules + cfg.hp_max, mx)) {
        if (hp >= 0 && hp <= 500000 && mx >= 1 && mx <= 500000) {
            out = modules;
            return true;
        }
    }
    return false;
}

bool LooksLikePlayer(uintptr_t chr, const Config& cfg) {
    if (!chr || (chr & 0x7) != 0) return false;
    uintptr_t data = 0;
    return ResolveCharData(chr, cfg, data);
}

}  // namespace

Mode ParseMode(const std::string& s) {
    const auto l = Lower(s);
    if (l == "random" || l == "rand" || l == "one") return Mode::Random;
    return Mode::All;
}

std::vector<PlayerRef> CollectPlayers(const Config& cfg) {
    std::vector<PlayerRef> out;
    std::unordered_set<uintptr_t> seen;

    auto world = ResolveWorldChrMan(cfg);
    if (!world) {
        log::Error("CollectPlayers: WorldChrMan missing");
        return out;
    }

    auto add = [&](uintptr_t chr, int slot, bool local) {
        if (!chr || seen.count(chr)) return;
        if (!LooksLikePlayer(chr, cfg)) return;
        PlayerRef p;
        p.chr = chr;
        p.slot = slot;
        p.is_local = local;
        ResolveCharData(chr, cfg, p.char_data);
        seen.insert(chr);
        out.push_back(p);
    };

    // Local player (TGA / Hexinton)
    uintptr_t local = 0;
    if (mem::ReadPtr(*world + 0x1E508, local) && local) {
        add(local, -1, true);
    }

    // NetPlayers array: WorldChrMan+0x10EF8 -> table of entries stride 0x10
    // Entry layout varies; try pointer at +0 of each slot (TGA getPlayerIns).
    uintptr_t net = 0;
    if (mem::ReadPtr(*world + cfg.worldchr_player_offset, net) && net) {
        for (int i = 0; i < 6; ++i) {
            uintptr_t entry = 0;
            // Common: array of PlayerIns* at stride 0x10
            if (!mem::ReadPtr(net + static_cast<uintptr_t>(i) * 0x10, entry) || !entry) {
                // Alternate: entry is a small struct, ChrIns at +0x0 already tried; try +0x8
                uintptr_t alt = 0;
                if (mem::ReadPtr(net + static_cast<uintptr_t>(i) * 0x10 + 0x8, alt) && alt) {
                    entry = alt;
                } else {
                    continue;
                }
            }
            add(entry, i, false);
        }
    }

    // If still empty, fall back to NetPlayers[0] chain used by heal
    if (out.empty() && net) {
        uintptr_t p0 = 0;
        if (mem::ReadPtr(net, p0) && p0) add(p0, 0, true);
    }

    log::Info("CollectPlayers found " + std::to_string(out.size()) + " player(s)");
    return out;
}

std::vector<PlayerRef> SelectTargets(const Config& cfg, Mode mode) {
    auto all = CollectPlayers(cfg);
    if (all.empty()) return all;

    if (mode == Mode::Local) {
        for (const auto& p : all) {
            if (p.is_local) return {p};
        }
        return {all[0]};
    }

    if (mode == Mode::All || all.size() == 1) return all;

    // Random among currently connected players (works for 1..N)
    static bool seeded = false;
    if (!seeded) {
        std::srand(static_cast<unsigned>(std::time(nullptr)) ^ 0xC4A05u);
        seeded = true;
    }
    const int idx = std::rand() % static_cast<int>(all.size());
    return {all[static_cast<size_t>(idx)]};
}

std::string DescribeTargets(const std::vector<PlayerRef>& targets) {
    if (targets.empty()) return "nobody";
    if (targets.size() == 1) {
        return targets[0].is_local ? "host" : ("slot" + std::to_string(targets[0].slot));
    }
    return std::to_string(targets.size()) + " players";
}

}  // namespace chaos::targets
