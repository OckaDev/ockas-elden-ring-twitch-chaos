#include "game/memory.hpp"
#include "util/log.hpp"

#include <Windows.h>
#include <Psapi.h>

#include <cstring>
#include <optional>
#include <sstream>
#include <vector>

#pragma comment(lib, "Psapi.lib")

namespace chaos::mem {
namespace {

struct PatternByte {
    uint8_t value = 0;
    bool wildcard = false;
};

std::vector<PatternByte> ParsePattern(std::string_view pattern) {
    std::vector<PatternByte> out;
    std::stringstream ss{std::string(pattern)};
    std::string tok;
    while (ss >> tok) {
        if (tok == "?" || tok == "??") {
            out.push_back({0, true});
        } else {
            out.push_back({static_cast<uint8_t>(std::stoul(tok, nullptr, 16)), false});
        }
    }
    return out;
}

}  // namespace

uintptr_t ModuleBase() {
    return reinterpret_cast<uintptr_t>(GetModuleHandleW(L"eldenring.exe"));
}

size_t ModuleSize() {
    HMODULE mod = GetModuleHandleW(L"eldenring.exe");
    if (!mod) return 0;
    MODULEINFO info{};
    if (!GetModuleInformation(GetCurrentProcess(), mod, &info, sizeof(info))) return 0;
    return info.SizeOfImage;
}

namespace {

std::vector<uintptr_t> ScanAob(std::string_view pattern, size_t max_hits) {
    std::vector<uintptr_t> hits;
    auto bytes = ParsePattern(pattern);
    if (bytes.empty()) return hits;

    const uintptr_t base = ModuleBase();
    const size_t size = ModuleSize();
    if (!base || !size) {
        log::Error("eldenring.exe module not found");
        return hits;
    }
    if (size < bytes.size()) return hits;

    const auto* start = reinterpret_cast<const uint8_t*>(base);
    const size_t last = size - bytes.size();

    for (size_t i = 0; i <= last; ++i) {
        bool ok = true;
        for (size_t j = 0; j < bytes.size(); ++j) {
            if (bytes[j].wildcard) continue;
            if (start[i + j] != bytes[j].value) {
                ok = false;
                break;
            }
        }
        if (!ok) continue;
        hits.push_back(base + i);
        if (hits.size() >= max_hits) break;
    }
    return hits;
}

}  // namespace

std::optional<uintptr_t> FindAob(std::string_view pattern) {
    auto hits = ScanAob(pattern, 1);
    if (hits.empty()) return std::nullopt;
    return hits[0];
}

std::optional<uintptr_t> FindUniqueAob(std::string_view pattern) {
    auto hits = ScanAob(pattern, 3);
    if (hits.size() == 1) return hits[0];
    return std::nullopt;
}

std::optional<uintptr_t> ResolveRip(uintptr_t instruction, int offset_at, int instr_size) {
    int32_t rel = 0;
    if (!Read(instruction + offset_at, rel)) return std::nullopt;
    return instruction + instr_size + rel;
}

bool ReadPtr(uintptr_t address, uintptr_t& out) {
    return Read(address, out);
}

bool ReadBytes(uintptr_t address, void* out, size_t len) {
    if (!address || !out || !len) return false;
    __try {
        std::memcpy(out, reinterpret_cast<const void*>(address), len);
        return true;
    } __except (1) {
        return false;
    }
}

bool WriteBytes(uintptr_t address, const void* data, size_t len) {
    if (!address || !data || !len) return false;
    __try {
        std::memcpy(reinterpret_cast<void*>(address), data, len);
        return true;
    } __except (1) {
        return false;
    }
}

}  // namespace chaos::mem
