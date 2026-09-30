#pragma once

#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace chaos::mem {

uintptr_t ModuleBase();
size_t ModuleSize();

std::optional<uintptr_t> FindAob(std::string_view pattern);
// First match only if the pattern hits exactly once (safer for short ItemGive-style AOBs).
std::optional<uintptr_t> FindUniqueAob(std::string_view pattern);
std::optional<uintptr_t> ResolveRip(uintptr_t instruction, int offset_at, int instr_size);

bool ReadBytes(uintptr_t address, void* out, size_t len);
bool WriteBytes(uintptr_t address, const void* data, size_t len);
bool ReadPtr(uintptr_t address, uintptr_t& out);

template <typename T>
bool Read(uintptr_t address, T& out) {
    return ReadBytes(address, &out, sizeof(T));
}

template <typename T>
bool Write(uintptr_t address, const T& value) {
    return WriteBytes(address, &value, sizeof(T));
}

}  // namespace chaos::mem
