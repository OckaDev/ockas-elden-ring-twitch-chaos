#include "command_router.hpp"
#include "net_sync.hpp"
#include "udp_server.hpp"
#include "util/config.hpp"
#include "util/log.hpp"

#include <Windows.h>

#include <memory>

namespace {
std::unique_ptr<chaos::UdpServer> g_server;

DWORD WINAPI InitThread(LPVOID) {
    // Held for the life of the process. A second game copy on the same PC sees it
    // and becomes a local test friend instead of a second host.
    HANDLE instance = CreateMutexW(nullptr, FALSE, L"Local\\EldenChaosInstance");
    const bool second_copy = instance && GetLastError() == ERROR_ALREADY_EXISTS;

    chaos::log::Init(second_copy ? "elden_chaos_test_friend.log" : "elden_chaos.log");
    chaos::log::Info("EldenChaos init thread starting");

    // Give the game a moment when loaded via Mod Engine 2 / Seamless
    Sleep(3000);

    chaos::Config cfg;
    chaos::LoadConfig(cfg);

    // Streamer.bot events belong to the first copy only.
    if (!second_copy) {
        g_server = std::make_unique<chaos::UdpServer>(cfg.udp_port);
        g_server->Start([](const std::string& payload) {
            chaos::HandleCommandJson(payload);
        });
    }
    chaos::net::Start(second_copy);
    return 0;
}
}  // namespace

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        HANDLE t = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
    } else if (reason == DLL_PROCESS_DETACH) {
        chaos::net::Stop();
        if (g_server) {
            g_server->Stop();
            g_server.reset();
        }
    }
    return TRUE;
}
