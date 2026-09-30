#include "udp_server.hpp"
#include "util/log.hpp"

#include <WinSock2.h>
#include <WS2tcpip.h>

#include <vector>

#pragma comment(lib, "ws2_32.lib")

namespace chaos {

UdpServer::UdpServer(int port) : port_(port) {}

UdpServer::~UdpServer() { Stop(); }

void UdpServer::Start(Handler handler) {
    if (running_.exchange(true)) return;

    thread_ = std::thread([this, handler = std::move(handler)]() {
        WSADATA wsa{};
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            log::Error("WSAStartup failed");
            running_ = false;
            return;
        }

        SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock == INVALID_SOCKET) {
            log::Error("UDP socket() failed");
            WSACleanup();
            running_ = false;
            return;
        }

        BOOL yes = TRUE;
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));

        // Bind all interfaces so Streamer.bot BroadcastUdp packets are received
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<u_short>(port_));
        addr.sin_addr.s_addr = htonl(INADDR_ANY);

        if (bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
            const int err = WSAGetLastError();
            log::Error(
                "UDP bind failed on port " + std::to_string(port_) +
                " (WSA " + std::to_string(err) +
                "). Close tools/mock_game_listener.py if it is still running.");
            closesocket(sock);
            WSACleanup();
            running_ = false;
            return;
        }

        // Non-blocking-ish receive loop with timeout so Stop() can exit.
        DWORD timeout = 500;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));

        log::Info("UDP listening on 127.0.0.1:" + std::to_string(port_));
        std::vector<char> buf(8192);

        while (running_) {
            sockaddr_in from{};
            int fromLen = sizeof(from);
            const int n = recvfrom(sock, buf.data(), static_cast<int>(buf.size()), 0,
                                   reinterpret_cast<sockaddr*>(&from), &fromLen);
            if (n == SOCKET_ERROR) {
                const int err = WSAGetLastError();
                if (err == WSAETIMEDOUT || err == WSAEWOULDBLOCK) continue;
                if (!running_) break;
                log::Warn("recvfrom error " + std::to_string(err));
                continue;
            }
            if (n <= 0) continue;
            try {
                handler(std::string(buf.data(), buf.data() + n));
            } catch (const std::exception& ex) {
                log::Error(std::string("handler exception: ") + ex.what());
            } catch (...) {
                log::Error("handler unknown exception");
            }
        }

        closesocket(sock);
        WSACleanup();
        log::Info("UDP server stopped");
    });
}

void UdpServer::Stop() {
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
}

}  // namespace chaos
