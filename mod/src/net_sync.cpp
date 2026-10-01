#include "net_sync.hpp"
#include "command_router.hpp"
#include "util/config.hpp"
#include "util/log.hpp"

#include <WinSock2.h>
#include <WS2tcpip.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "ws2_32.lib")

namespace chaos::net {
namespace {

struct NetConfig {
    bool client = false;
    std::string host;
    int port = 22667;
    std::string secret;
};

std::atomic<bool> g_running{false};
std::atomic<bool> g_client_mode{false};
std::mutex g_mu;
std::mutex g_send_mu;
std::vector<SOCKET> g_clients;
SOCKET g_listen = INVALID_SOCKET;
SOCKET g_host_sock = INVALID_SOCKET;

std::string Trim(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    return s.substr(i);
}

std::string Lower(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

std::string RandomSecret() {
    static const char chars[] = "abcdefghjkmnpqrstuvwxyz23456789";
    std::random_device rd;
    std::string out;
    for (int i = 0; i < 12; ++i) out.push_back(chars[rd() % (sizeof(chars) - 1)]);
    return out;
}

NetConfig LoadNetConfig() {
    NetConfig cfg;
    const std::string path = DllDirectory() + "chaos_net.ini";

    std::ifstream in(path);
    if (!in) {
        // First run: create a host config with a fresh secret.
        cfg.secret = RandomSecret();
        std::ofstream out(path);
        out << "# EldenChaos multiplayer link\n"
               "# HOST (streamer): leave 'host=' empty. Friends connect to your LAN or VPN address.\n"
               "# FRIEND: set host=<host address>, use the SAME secret. Do not share this file publicly.\n"
               "host=\n"
               "port=22667\n"
               "secret=" << cfg.secret << "\n";
        log::Info("Created " + path + " (host mode)");
        return cfg;
    }

    std::string line;
    std::unordered_map<std::string, std::string> kv;
    while (std::getline(in, line)) {
        line = Trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        kv[Lower(Trim(line.substr(0, eq)))] = Trim(line.substr(eq + 1));
    }
    cfg.host = kv["host"];
    cfg.secret = kv["secret"];
    if (!kv["port"].empty()) {
        try { cfg.port = std::stoi(kv["port"]); } catch (...) {}
    }
    cfg.client = !cfg.host.empty();
    return cfg;
}

bool SendLine(SOCKET s, const std::string& line) {
    const std::string data = line + "\n";
    size_t sent = 0;
    while (sent < data.size()) {
        const int n = send(s, data.data() + sent, static_cast<int>(data.size() - sent), 0);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

// Reads one '\n'-terminated line. Returns false on error/timeout/close.
bool RecvLine(SOCKET s, std::string& out, std::string& buf) {
    while (true) {
        const auto nl = buf.find('\n');
        if (nl != std::string::npos) {
            out = Trim(buf.substr(0, nl));
            buf.erase(0, nl + 1);
            return true;
        }
        char tmp[1024];
        const int n = recv(s, tmp, sizeof(tmp), 0);
        if (n <= 0) return false;
        buf.append(tmp, static_cast<size_t>(n));
        if (buf.size() > 64 * 1024) return false;
    }
}

void SetTimeoutMs(SOCKET s, int ms) {
    DWORD t = static_cast<DWORD>(ms);
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&t), sizeof(t));
}

void SetKeepAlive(SOCKET s) {
    BOOL on = TRUE;
    setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, reinterpret_cast<const char*>(&on), sizeof(on));
}

void SetNoDelay(SOCKET s) {
    BOOL on = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&on), sizeof(on));
}

bool SendLineLocked(SOCKET s, const std::string& line) {
    std::lock_guard lock(g_send_mu);
    return SendLine(s, line);
}

void HostAcceptLoop(NetConfig cfg) {
    g_listen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_listen == INVALID_SOCKET) {
        log::Error("Net: listen socket() failed");
        return;
    }
    BOOL yes = TRUE;
    setsockopt(g_listen, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<u_short>(cfg.port));
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(g_listen, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        log::Error("Net: bind TCP " + std::to_string(cfg.port) + " failed err=" +
                   std::to_string(WSAGetLastError()));
        closesocket(g_listen);
        g_listen = INVALID_SOCKET;
        return;
    }
    listen(g_listen, 4);
    log::Info("Net: host listening on TCP " + std::to_string(cfg.port));

    while (g_running) {
        sockaddr_in peer{};
        int plen = sizeof(peer);
        SOCKET c = accept(g_listen, reinterpret_cast<sockaddr*>(&peer), &plen);
        if (c == INVALID_SOCKET) {
            if (!g_running) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }

        char ip[64] = {};
        inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));

        // Handshake on its own thread so a slow peer can't block accepts.
        std::thread([c, cfg, ip = std::string(ip)]() {
            SetTimeoutMs(c, 5000);
            std::string buf, line;
            const bool got = RecvLine(c, line, buf);
            if (!got || line != "HELLO " + cfg.secret || cfg.secret.empty()) {
                log::Warn("Net: rejected connection from " + ip);
                closesocket(c);
                return;
            }
            SendLineLocked(c, "OK");
            SetTimeoutMs(c, 0);
            SetKeepAlive(c);
            SetNoDelay(c);
            {
                std::lock_guard lock(g_mu);
                g_clients.push_back(c);
            }
            log::Info("Net: friend connected from " + ip + " (clients=" +
                      std::to_string(ClientCount()) + ")");

            std::string inbuf, msg;
            while (g_running && RecvLine(c, msg, inbuf)) {
                if (msg.empty() || msg == "PING") continue;
                HandleRemoteCommandJson(msg);
            }
            {
                std::lock_guard lock(g_mu);
                for (auto it = g_clients.begin(); it != g_clients.end(); ++it) {
                    if (*it == c) {
                        g_clients.erase(it);
                        break;
                    }
                }
            }
            closesocket(c);
            log::Info("Net: friend disconnected");
        }).detach();
    }
}

void HostHeartbeatLoop() {
    while (g_running) {
        for (int i = 0; i < 50 && g_running; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        Broadcast("PING");  // also prunes dead sockets
    }
}

void ClientLoop(NetConfig cfg) {
    log::Info("Net: client mode, host=" + cfg.host + ":" + std::to_string(cfg.port));
    int attempt = 0;
    while (g_running) {
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* res = nullptr;
        SOCKET s = INVALID_SOCKET;

        ++attempt;
        const bool verbose = (attempt == 1 || attempt % 12 == 0);
        if (getaddrinfo(cfg.host.c_str(), std::to_string(cfg.port).c_str(), &hints, &res) == 0 && res) {
            s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
            if (s != INVALID_SOCKET) {
                SetTimeoutMs(s, 5000);
                if (connect(s, res->ai_addr, static_cast<int>(res->ai_addrlen)) != 0) {
                    if (verbose) {
                        const int err = WSAGetLastError();
                        log::Warn("Net: connect to host failed (attempt " + std::to_string(attempt) +
                                  ", winsock error " + std::to_string(err) +
                                  (err == 10060 ? " = timed out: port not reachable / forward or firewall blocking"
                                   : err == 10061 ? " = refused: nothing listening on that port"
                                                  : "") + ")");
                    }
                    closesocket(s);
                    s = INVALID_SOCKET;
                }
            }
            freeaddrinfo(res);
        } else if (verbose) {
            log::Warn("Net: could not resolve host '" + cfg.host + "'");
        }

        if (s != INVALID_SOCKET) {
            std::string buf, line;
            bool ok = SendLine(s, "HELLO " + cfg.secret) && RecvLine(s, line, buf) && line == "OK";
            if (ok) {
                log::Info("Net: connected to host");
                SetKeepAlive(s);
                SetNoDelay(s);
                SetTimeoutMs(s, 30000);  // host pings every 5s
                {
                    std::lock_guard lock(g_mu);
                    g_host_sock = s;
                }
                while (g_running && RecvLine(s, line, buf)) {
                    if (line.empty() || line == "PING") continue;
                    HandleRemoteCommandJson(line);
                }
                {
                    std::lock_guard lock(g_mu);
                    g_host_sock = INVALID_SOCKET;
                }
                log::Warn("Net: lost connection to host");
            } else {
                log::Warn("Net: host rejected handshake (wrong secret?)");
            }
            closesocket(s);
        }

        for (int i = 0; i < 50 && g_running; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}

}  // namespace

void Start(bool local_test) {
    if (g_running.exchange(true)) return;

    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);

    NetConfig cfg = LoadNetConfig();
    if (local_test && !cfg.client) {
        cfg.client = true;
        cfg.host = "127.0.0.1";
        log::Info("Net: second game copy on this PC, joining the first one as a test friend");
    }
    g_client_mode = cfg.client;

    if (cfg.client) {
        std::thread(ClientLoop, cfg).detach();
    } else {
        std::thread(HostAcceptLoop, cfg).detach();
        std::thread(HostHeartbeatLoop).detach();
    }
}

void Stop() {
    g_running = false;
    if (g_listen != INVALID_SOCKET) {
        closesocket(g_listen);
        g_listen = INVALID_SOCKET;
    }
    std::lock_guard lock(g_mu);
    g_host_sock = INVALID_SOCKET;
    for (SOCKET c : g_clients) closesocket(c);
    g_clients.clear();
}

bool IsClient() { return g_client_mode; }

int ClientCount() {
    std::lock_guard lock(g_mu);
    return static_cast<int>(g_clients.size());
}

void Broadcast(const std::string& json) {
    std::lock_guard lock(g_mu);
    for (auto it = g_clients.begin(); it != g_clients.end();) {
        if (!SendLineLocked(*it, json)) {
            closesocket(*it);
            it = g_clients.erase(it);
            log::Info("Net: friend disconnected");
        } else {
            ++it;
        }
    }
}

bool SendToClient(int index, const std::string& json) {
    std::lock_guard lock(g_mu);
    if (index < 0 || index >= static_cast<int>(g_clients.size())) return false;
    if (!SendLineLocked(g_clients[static_cast<size_t>(index)], json)) {
        closesocket(g_clients[static_cast<size_t>(index)]);
        g_clients.erase(g_clients.begin() + index);
        log::Info("Net: friend disconnected");
        return false;
    }
    return true;
}

bool SendToHost(const std::string& json) {
    SOCKET s = INVALID_SOCKET;
    {
        std::lock_guard lock(g_mu);
        s = g_host_sock;
    }
    if (s == INVALID_SOCKET) return false;
    return SendLineLocked(s, json);
}

}  // namespace chaos::net
