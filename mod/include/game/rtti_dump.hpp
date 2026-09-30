#pragma once

#include <string>
#include <vector>

namespace chaos::rtti {

// Diagnostic: scans eldenring.exe RTTI for classes whose name contains any keyword and logs
// their vtables to elden_chaos.log. Runs on a background thread.
void DumpClasses(const std::vector<std::string>& keywords, size_t vtable_entries = 24);

}  // namespace chaos::rtti
