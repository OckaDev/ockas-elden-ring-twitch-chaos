#pragma once

#include <string>
#include <string_view>

namespace chaos::log {

void Init(const char* file_name = "elden_chaos.log");
void Info(std::string_view msg);
void Warn(std::string_view msg);
void Error(std::string_view msg);

}  // namespace chaos::log
