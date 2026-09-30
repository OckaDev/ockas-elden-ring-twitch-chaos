#include "util/log.hpp"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <string>

#include <Windows.h>

namespace chaos::log {
namespace {

std::mutex g_mu;
std::ofstream g_file;
bool g_ready = false;

std::string Timestamp() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%02u:%02u:%02u", st.wHour, st.wMinute, st.wSecond);
    return buf;
}

void Write(std::string_view level, std::string_view msg) {
    std::lock_guard lock(g_mu);
    if (!g_ready) return;
    g_file << "[" << Timestamp() << "][" << level << "] " << msg << std::endl;
    OutputDebugStringA(("[EldenChaos][" + std::string(level) + "] " + std::string(msg) + "\n").c_str());
}

}  // namespace

void Init() {
    std::lock_guard lock(g_mu);
    char path[MAX_PATH]{};
    HMODULE self = nullptr;
    GetModuleHandleExA(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCSTR>(&Init),
        &self);
    GetModuleFileNameA(self, path, MAX_PATH);
    std::string p(path);
    auto slash = p.find_last_of("\\/");
    if (slash != std::string::npos) p = p.substr(0, slash + 1);
    const std::string dir = p;
    p += "elden_chaos.log";
    g_file.open(p, std::ios::app);
    g_ready = g_file.is_open();
    if (!g_ready) {
        // Game folder not writable (e.g. protected install) — fall back to %TEMP%.
        char tmp[MAX_PATH]{};
        if (GetTempPathA(MAX_PATH, tmp) > 0) {
            g_file.clear();
            g_file.open(std::string(tmp) + "elden_chaos.log", std::ios::app);
            g_ready = g_file.is_open();
            if (g_ready) g_file << "(could not write to " << dir << ")\n";
        }
    }
    if (g_ready) {
        g_file << "\n==== EldenChaos session ====\n";
        g_file.flush();
    }
}

void Info(std::string_view msg) { Write("INFO", msg); }
void Warn(std::string_view msg) { Write("WARN", msg); }
void Error(std::string_view msg) { Write("ERROR", msg); }

}  // namespace chaos::log
