#include "game/params.hpp"
#include "game/memory.hpp"
#include "util/log.hpp"

#include <Windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace chaos::params {
namespace {

// SoloParamRepository: 194 holders of 0x48 bytes from +0x80, first res cap at +0x08.
constexpr uintptr_t kHolders = 0x80;
constexpr uintptr_t kHolderSize = 0x48;
constexpr int kHolderCount = 194;
constexpr uintptr_t kResCapToFd4 = 0x80;
constexpr uintptr_t kFd4ToFile = 0x80;
constexpr uintptr_t kFileRowCount = 0x0A;
constexpr uintptr_t kFileRows = 0x40;
constexpr uintptr_t kRowRecord = 0x18;

std::string Hex(uintptr_t v) {
    char b[32];
    std::snprintf(b, sizeof(b), "%llX", static_cast<unsigned long long>(v));
    return b;
}

bool InImage(uintptr_t a) {
    const uintptr_t b = mem::ModuleBase();
    const size_t n = mem::ModuleSize();
    return b && n && a >= b && a < b + n;
}

bool PlausibleHeap(uintptr_t v) {
    return v >= 0x10000 && v < 0x00007FFFFFFFFFFFull && (v & 7) == 0 && !InImage(v);
}

struct ParamFile {
    uintptr_t file = 0;
    uint16_t rows = 0;
    uint32_t row_size = 0;
};

bool ReadParamFile(uintptr_t repo, int index, ParamFile& out) {
    uintptr_t rescap = 0, fd4 = 0, file = 0;
    const uintptr_t holder = repo + kHolders + static_cast<uintptr_t>(index) * kHolderSize;
    if (!mem::ReadPtr(holder + 8, rescap) || !rescap) return false;
    if (!mem::ReadPtr(rescap + kResCapToFd4, fd4) || !fd4) return false;
    if (!mem::ReadPtr(fd4 + kFd4ToFile, file) || !file) return false;
    uint16_t rows = 0;
    if (!mem::Read(file + kFileRowCount, rows) || rows == 0) return false;
    uint64_t d0 = 0, d1 = 0;
    if (!mem::Read(file + kFileRows + 8, d0)) return false;
    if (rows >= 2 && !mem::Read(file + kFileRows + kRowRecord + 8, d1)) return false;
    out.file = file;
    out.rows = rows;
    out.row_size = (rows >= 2 && d1 > d0) ? static_cast<uint32_t>(d1 - d0) : 0;
    return true;
}

uintptr_t FindRowIn(const ParamFile& pf, uint32_t id) {
    int lo = 0, hi = static_cast<int>(pf.rows) - 1;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        const uintptr_t rec = pf.file + kFileRows + static_cast<uintptr_t>(mid) * kRowRecord;
        uint32_t rid = 0;
        if (!mem::Read(rec, rid)) return 0;
        if (rid == id) {
            uint64_t off = 0;
            if (!mem::Read(rec + 8, off) || !off) return 0;
            return pf.file + static_cast<uintptr_t>(off);
        }
        if (rid < id) lo = mid + 1;
        else hi = mid - 1;
    }
    return 0;
}

struct Fingerprint {
    Table table;
    const char* name;
    uint32_t row_size;
    uint32_t known_ids[3];
};

// Several tables share a row size, so each needs rows only it has.
const Fingerprint kPrints[] = {
    {Table::Weapon, "EquipParamWeapon", 0x298, {110000, 1000000, 0}},
    {Table::Gem, "EquipParamGem", 0x60, {10000, 85000, 800000}},
    {Table::Npc, "NpcParam", 0x2E0, {21300014, 0, 0}},
    {Table::ItemLotEnemy, "ItemLotParam_enemy", 0x98, {227000000, 0, 0}},
};

std::mutex g_mu;
uintptr_t g_repo_slot = 0;
bool g_repo_searched = false;
std::unordered_map<int, int> g_table_index;  // Table -> holder index

uintptr_t Repo() {
    if (!g_repo_searched) {
        g_repo_searched = true;
        g_repo_slot = FindSingletonSlot("SoloParamRepository", [](uintptr_t obj) {
            uint32_t count = 0;
            uintptr_t first = 0;
            return mem::Read(obj + kHolders, count) && count >= 1 && count <= 8 &&
                   mem::ReadPtr(obj + kHolders + 8, first) && first != 0;
        });
        if (g_repo_slot) log::Info("Params: SoloParamRepository slot=base+0x" + Hex(g_repo_slot - mem::ModuleBase()));
        else log::Error("Params: SoloParamRepository not found");
    }
    if (!g_repo_slot) return 0;
    uintptr_t repo = 0;
    mem::ReadPtr(g_repo_slot, repo);
    return repo;
}

bool Resolve(Table table, ParamFile& out) {
    std::lock_guard lock(g_mu);
    const uintptr_t repo = Repo();
    if (!repo) return false;
    auto it = g_table_index.find(static_cast<int>(table));
    if (it != g_table_index.end()) {
        if (it->second < 0) return false;
        return ReadParamFile(repo, it->second, out);
    }
    const Fingerprint* fp = nullptr;
    for (const auto& p : kPrints) {
        if (p.table == table) fp = &p;
    }
    if (!fp) return false;
    for (int i = 0; i < kHolderCount; ++i) {
        ParamFile pf;
        if (!ReadParamFile(repo, i, pf) || pf.row_size != fp->row_size) continue;
        bool all = true;
        for (uint32_t id : fp->known_ids) {
            if (id && !FindRowIn(pf, id)) all = false;
        }
        if (!all) continue;
        g_table_index[static_cast<int>(table)] = i;
        log::Info(std::string("Params: ") + fp->name + " = holder " + std::to_string(i) + " rows=" +
                  std::to_string(pf.rows));
        out = pf;
        return true;
    }
    g_table_index[static_cast<int>(table)] = -1;
    log::Error(std::string("Params: ") + fp->name + " not found");
    return false;
}

}  // namespace

bool RttiNameContains(uintptr_t obj, const char* part) {
    uintptr_t vt = 0;
    if (!mem::ReadPtr(obj, vt) || !InImage(vt)) return false;
    uintptr_t col = 0;
    if (!mem::ReadPtr(vt - 8, col) || !InImage(col)) return false;
    uint32_t sig = 0, td = 0;
    if (!mem::Read(col, sig) || sig != 1) return false;
    if (!mem::Read(col + 0x0C, td) || td == 0) return false;
    char buf[128]{};
    if (!mem::ReadBytes(mem::ModuleBase() + td + 0x10, buf, sizeof(buf) - 1)) return false;
    return std::strstr(buf, part) != nullptr;
}

uintptr_t FindSingletonSlot(const char* rtti_part, const std::function<bool(uintptr_t obj)>& validate) {
    const uintptr_t base = mem::ModuleBase();
    if (!base) return 0;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);

    std::unordered_map<uintptr_t, bool> vt_match;
    std::vector<uint8_t> buf;
    for (WORD s = 0; s < nt->FileHeader.NumberOfSections; ++s, ++sec) {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_WRITE)) continue;
        if (sec->Characteristics & IMAGE_SCN_MEM_EXECUTE) continue;
        const uintptr_t start = base + sec->VirtualAddress;
        const size_t size = sec->Misc.VirtualSize;
        constexpr size_t kChunk = 1 << 20;
        buf.resize(kChunk);
        for (size_t off = 0; off < size; off += kChunk) {
            const size_t n = std::min(kChunk, size - off) & ~static_cast<size_t>(7);
            if (!n || !mem::ReadBytes(start + off, buf.data(), n)) continue;
            for (size_t i = 0; i + 8 <= n; i += 8) {
                uintptr_t obj = 0;
                std::memcpy(&obj, buf.data() + i, 8);
                if (!PlausibleHeap(obj)) continue;
                uintptr_t vt = 0;
                if (!mem::ReadPtr(obj, vt) || !InImage(vt)) continue;
                auto it = vt_match.find(vt);
                bool match;
                if (it == vt_match.end()) {
                    match = RttiNameContains(obj, rtti_part);
                    vt_match.emplace(vt, match);
                } else {
                    match = it->second;
                }
                if (!match) continue;
                if (validate && !validate(obj)) continue;
                return start + off + i;
            }
        }
    }
    return 0;
}

uintptr_t Row(Table table, uint32_t id) {
    ParamFile pf;
    if (!Resolve(table, pf)) return 0;
    return FindRowIn(pf, id);
}

void ForEachRow(Table table, const std::function<void(uint32_t id, uintptr_t row)>& fn) {
    ParamFile pf;
    if (!Resolve(table, pf)) return;
    for (uint16_t i = 0; i < pf.rows; ++i) {
        const uintptr_t rec = pf.file + kFileRows + static_cast<uintptr_t>(i) * kRowRecord;
        uint32_t id = 0;
        uint64_t off = 0;
        if (!mem::Read(rec, id) || !mem::Read(rec + 8, off) || !off) continue;
        fn(id, pf.file + static_cast<uintptr_t>(off));
    }
}

}  // namespace chaos::params
