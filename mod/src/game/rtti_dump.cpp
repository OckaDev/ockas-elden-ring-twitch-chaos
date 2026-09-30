#include "game/rtti_dump.hpp"
#include "game/memory.hpp"
#include "util/log.hpp"

#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace chaos::rtti {
namespace {

struct Region {
    uintptr_t start;
    size_t size;
};

std::string Hex(uintptr_t v) {
    char b[32];
    std::snprintf(b, sizeof(b), "%llX", static_cast<unsigned long long>(v));
    return b;
}

std::vector<Region> ReadableRegions(uintptr_t base, size_t size) {
    std::vector<Region> out;
    uintptr_t p = base;
    const uintptr_t end = base + size;
    while (p < end) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<void*>(p), &mbi, sizeof(mbi))) break;
        const uintptr_t rs = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        const size_t rsz = mbi.RegionSize;
        const DWORD prot = mbi.Protect;
        const bool readable = (mbi.State == MEM_COMMIT) && !(prot & PAGE_GUARD) &&
                              !(prot & PAGE_NOACCESS) &&
                              (prot & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                                       PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                                       PAGE_EXECUTE_WRITECOPY));
        if (readable) out.push_back({std::max(rs, base), std::min(rs + rsz, end) - std::max(rs, base)});
        p = rs + rsz;
    }
    return out;
}

bool LowerContains(const std::string& hay, const std::string& needle) {
    auto it = std::search(hay.begin(), hay.end(), needle.begin(), needle.end(), [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
    });
    return it != hay.end();
}

// Copy a region into a local buffer chunk by chunk (SEH-safe via mem::ReadBytes).
template <typename Fn>
void ForEachChunk(const std::vector<Region>& regions, Fn&& fn) {
    constexpr size_t kChunk = 4 * 1024 * 1024;
    constexpr size_t kOverlap = 64;
    std::vector<uint8_t> buf(kChunk + kOverlap);
    for (const auto& r : regions) {
        size_t off = 0;
        while (off < r.size) {
            const size_t n = std::min(kChunk + kOverlap, r.size - off);
            if (!mem::ReadBytes(r.start + off, buf.data(), n)) {
                // Page-level fallback
                size_t good = 0;
                for (size_t pg = 0; pg < n; pg += 4096) {
                    const size_t len = std::min<size_t>(4096, n - pg);
                    if (mem::ReadBytes(r.start + off + pg, buf.data() + pg, len)) good = pg + len;
                    else std::memset(buf.data() + pg, 0, len);
                }
                (void)good;
            }
            fn(r.start + off, buf.data(), n);
            if (off + kChunk >= r.size) break;
            off += kChunk;
        }
    }
}

}  // namespace

void DumpClasses(const std::vector<std::string>& keywords, size_t vtable_entries) {
    std::thread([keywords, vtable_entries]() {
        const uintptr_t base = mem::ModuleBase();
        const size_t size = mem::ModuleSize();
        if (!base || !size) {
            log::Error("RTTI: module not found");
            return;
        }
        log::Info("RTTI: scanning eldenring.exe base=0x" + Hex(base) + " size=0x" + Hex(size));
        const auto regions = ReadableRegions(base, size);

        // Pass 1: find ".?AV...@@" type names matching keywords.
        struct Cls {
            std::string name;
            uintptr_t type_desc;  // address of TypeDescriptor
        };
        std::vector<Cls> classes;
        std::unordered_set<uintptr_t> seen_name;
        ForEachChunk(regions, [&](uintptr_t addr, const uint8_t* d, size_t n) {
            for (size_t i = 0; i + 8 < n; ++i) {
                if (d[i] != '.' || d[i + 1] != '?' || d[i + 2] != 'A' || d[i + 3] != 'V') continue;
                size_t j = i;
                while (j < n && j - i < 200 && d[j] >= 0x20 && d[j] < 0x7F) ++j;
                if (j >= n || d[j] != 0 || j - i < 8) continue;
                std::string name(reinterpret_cast<const char*>(d + i), j - i);
                bool match = false;
                for (const auto& k : keywords) {
                    if (LowerContains(name, k)) { match = true; break; }
                }
                if (!match) continue;
                const uintptr_t a = addr + i;
                if (seen_name.insert(a).second && classes.size() < 60) {
                    classes.push_back({name, a - 0x10});
                }
                i = j;
            }
        });
        log::Info("RTTI: " + std::to_string(classes.size()) + " class name(s) matched");
        if (classes.empty()) return;

        std::unordered_map<uint32_t, size_t> rva_to_class;
        for (size_t i = 0; i < classes.size(); ++i) {
            rva_to_class[static_cast<uint32_t>(classes[i].type_desc - base)] = i;
        }

        // Pass 2: Complete Object Locators referencing those type descriptors.
        std::unordered_map<uintptr_t, size_t> col_to_class;
        ForEachChunk(regions, [&](uintptr_t addr, const uint8_t* d, size_t n) {
            for (size_t i = 0; i + 24 <= n; i += 4) {
                uint32_t v;
                std::memcpy(&v, d + i, 4);
                auto it = rva_to_class.find(v);
                if (it == rva_to_class.end()) continue;
                if (i < 12) continue;
                const uintptr_t col = addr + i - 12;
                uint32_t sig, self_rva;
                std::memcpy(&sig, d + i - 12, 4);
                std::memcpy(&self_rva, d + i + 8, 4);
                if (sig != 1) continue;
                if (self_rva != static_cast<uint32_t>(col - base)) continue;
                col_to_class[col] = it->second;
            }
        });
        log::Info("RTTI: " + std::to_string(col_to_class.size()) + " locator(s) found");

        // Pass 3: vtables (pointer to COL sits right before the first virtual function).
        struct Vt {
            size_t cls;
            uintptr_t vtable;
        };
        std::vector<Vt> vtables;
        ForEachChunk(regions, [&](uintptr_t addr, const uint8_t* d, size_t n) {
            for (size_t i = 0; i + 8 <= n; i += 8) {
                uintptr_t v;
                std::memcpy(&v, d + i, 8);
                auto it = col_to_class.find(v);
                if (it == col_to_class.end()) continue;
                vtables.push_back({it->second, addr + i + 8});
            }
        });

        for (const auto& vt : vtables) {
            std::string line = "RTTI vtable " + classes[vt.cls].name + " @ base+0x" +
                               Hex(vt.vtable - base) + " funcs:";
            log::Info(line);
            for (size_t k = 0; k < vtable_entries; ++k) {
                uintptr_t fn = 0;
                if (!mem::ReadPtr(vt.vtable + k * 8, fn)) break;
                if (fn < base || fn > base + size) break;
                uint8_t code[16] = {};
                mem::ReadBytes(fn, code, sizeof(code));
                char hex[64];
                std::snprintf(hex, sizeof(hex),
                              "%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
                              code[0], code[1], code[2], code[3], code[4], code[5],
                              code[6], code[7], code[8], code[9], code[10], code[11]);
                log::Info("   [" + std::to_string(k) + "] base+0x" + Hex(fn - base) + " : " + hex);
            }
        }
        log::Info("RTTI: dump finished");
    }).detach();
}

}  // namespace chaos::rtti
