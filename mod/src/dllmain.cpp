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
    chaos::log::Init();
    chaos::log::Info("EldenChaos init thread starting");

    // Give the game a moment when loaded via Mod Engine 2 / Seamless
    Sleep(3000);

    chaos::Config cfg;
    chaos::LoadConfig(cfg);

    g_server = std::make_unique<chaos::UdpServer>(cfg.udp_port);
    g_server->Start([](const std::string& payload) {
        chaos::HandleCommandJson(payload);
    });
    chaos::net::Start();
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
