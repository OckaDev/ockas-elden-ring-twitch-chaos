#pragma once

#include <cstdint>
#include <functional>

namespace chaos::params {

enum class Table { Weapon, Gem, Npc, ItemLotEnemy };

// Address of the row data for `id`, or 0 when the table or row is missing.
uintptr_t Row(Table table, uint32_t id);
void ForEachRow(Table table, const std::function<void(uint32_t id, uintptr_t row)>& fn);

// Scans eldenring.exe's writable sections for a static pointer to an object whose
// RTTI class name contains `rtti_part` and that passes `validate`. Returns the
// address of that pointer slot (re-read it, the object can be replaced).
uintptr_t FindSingletonSlot(const char* rtti_part, const std::function<bool(uintptr_t obj)>& validate);

bool RttiNameContains(uintptr_t obj, const char* part);

}  // namespace chaos::params
