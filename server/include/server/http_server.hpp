#pragma once

#include <string>
#include <thread>
#include <atomic>
#include <filesystem>
#include <unordered_map>

namespace Frontier::Server {

class HttpServer {
public:
    HttpServer() = default;
    ~HttpServer();

    bool start(const std::string& host, uint16_t port, const std::string& rootDirectory);
    void stop();

    bool isRunning() const { return m_running; }
    uint16_t getPort() const { return m_port; }

private:
    void listenLoop();

    std::string m_host{"0.0.0.0"};
    uint16_t m_port{4675};
    std::filesystem::path m_rootDir;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_ready{false};
    std::atomic<bool> m_failed{false};
    std::thread m_thread;
};

} // namespace Frontier::Server
