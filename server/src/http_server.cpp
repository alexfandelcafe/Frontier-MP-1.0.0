#include "server/http_server.hpp"
#include <iostream>
#include <fstream>
#include <sstream>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#define SOCKET int
#define INVALID_SOCKET -1
#define SOCKET_ERROR -1
#define closesocket close
#endif

namespace Frontier::Server {

HttpServer::~HttpServer() {
    stop();
}

bool HttpServer::start(const std::string& host, uint16_t port, const std::string& rootDirectory) {
    if (m_running) return true;

    m_host = host;
    m_port = port;
    m_rootDir = rootDirectory;
    m_running = true;

    m_thread = std::thread(&HttpServer::listenLoop, this);
    std::cout << "[HttpServer] Asset streaming server listening on http://" << host << ":" << port << std::endl;
    return true;
}

void HttpServer::stop() {
    if (!m_running) return;
    m_running = false;
    if (m_thread.joinable()) {
        m_thread.detach();
    }
}

void HttpServer::listenLoop() {
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

    SOCKET serverSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (serverSock == INVALID_SOCKET) {
        std::cerr << "[HttpServer] Failed to create socket" << std::endl;
        return;
    }

    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(m_port);
    serverAddr.sin_addr.s_addr = INADDR_ANY;

    if (bind(serverSock, reinterpret_cast<sockaddr*>(&serverAddr), sizeof(serverAddr)) == SOCKET_ERROR) {
        std::cerr << "[HttpServer] Failed to bind to port " << m_port << std::endl;
        closesocket(serverSock);
        return;
    }

    listen(serverSock, 16);

    while (m_running) {
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(serverSock, &readSet);

        timeval tv{1, 0}; // 1 sec timeout
        int sel = select(0, &readSet, nullptr, nullptr, &tv);
        if (sel > 0 && FD_ISSET(serverSock, &readSet)) {
            sockaddr_in clientAddr{};
            int clientLen = sizeof(clientAddr);
            SOCKET clientSock = accept(serverSock, reinterpret_cast<sockaddr*>(&clientAddr), &clientLen);
            if (clientSock != INVALID_SOCKET) {
                char buffer[2048] = {0};
                int bytes = recv(clientSock, buffer, sizeof(buffer) - 1, 0);
                if (bytes > 0) {
                    std::string req(buffer);
                    std::string response;

                    if (req.rfind("GET /status", 0) == 0) {
                        std::string body = "{\"status\":\"online\",\"server\":\"FrontierMP\"}";
                        response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
                                   std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
                    } else {
                        std::string body = "FrontierMP Asset Streaming Server";
                        response = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: " +
                                   std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
                    }
                    send(clientSock, response.c_str(), static_cast<int>(response.size()), 0);
                }
                closesocket(clientSock);
            }
        }
    }

    closesocket(serverSock);
#ifdef _WIN32
    WSACleanup();
#endif
}

} // namespace Frontier::Server
