#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <thread>

namespace chaos {

class UdpServer {
public:
    using Handler = std::function<void(const std::string& payload)>;

    explicit UdpServer(int port);
    ~UdpServer();

    void Start(Handler handler);
    void Stop();

private:
    int port_;
    std::atomic<bool> running_{false};
    std::thread thread_;
};

}  // namespace chaos
