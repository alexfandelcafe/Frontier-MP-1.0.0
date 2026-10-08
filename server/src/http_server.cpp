#include "server/http_server.hpp"

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <algorithm>
#include <cctype>

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

namespace {

std::string urlDecode(const std::string& value) {
    std::string out;
    out.reserve(value.size());

    auto hexValue = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };

    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '%' && i + 2 < value.size()) {
            const int hi = hexValue(value[i + 1]);
            const int lo = hexValue(value[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(
                    static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }

        out.push_back(value[i] == '+' ? ' ' : value[i]);
    }

    return out;
}

bool isSafeRelativePath(const std::filesystem::path& root,
                        const std::filesystem::path& candidate)
{
    std::error_code ec;
    const auto rootCanonical =
        std::filesystem::weakly_canonical(root, ec);
    if (ec) return false;

    const auto candidateCanonical =
        std::filesystem::weakly_canonical(candidate, ec);
    if (ec) return false;

    auto rootIt = rootCanonical.begin();
    auto rootEnd = rootCanonical.end();
    auto candidateIt = candidateCanonical.begin();
    auto candidateEnd = candidateCanonical.end();

    for (; rootIt != rootEnd; ++rootIt, ++candidateIt) {
        if (candidateIt == candidateEnd ||
            *rootIt != *candidateIt) {
            return false;
        }
    }

    return true;
}

std::string makeResponse(
    int status,
    const std::string& reason,
    const std::string& contentType,
    const std::string& body)
{
    std::ostringstream out;
    out << "HTTP/1.1 " << status << ' ' << reason << "\r\n"
        << "Content-Type: " << contentType << "\r\n"
        << "Content-Length: " << body.size() << "\r\n"
        << "Connection: close\r\n\r\n";
    out << body;
    return out.str();
}

std::string buildManifest(const std::filesystem::path& root) {
    std::ostringstream out;
    std::error_code ec;

    if (!std::filesystem::exists(root, ec)) {
        return {};
    }

    for (const auto& resourceEntry :
         std::filesystem::directory_iterator(root, ec)) {

        if (ec) break;
        if (!resourceEntry.is_directory()) continue;

        const std::string resource =
            resourceEntry.path().filename().string();

        for (const auto& fileEntry :
             std::filesystem::recursive_directory_iterator(
                 resourceEntry.path(), ec)) {

            if (ec) break;
            if (!fileEntry.is_regular_file()) continue;

            const auto relative =
                std::filesystem::relative(
                    fileEntry.path(),
                    resourceEntry.path(),
                    ec);

            if (ec) {
                ec.clear();
                continue;
            }

            out << resource
                << '\t'
                << relative.generic_string()
                << '\n';
        }
    }

    return out.str();
}

std::string guessContentType(
    const std::filesystem::path& path)
{
    const std::string ext =
        path.extension().string();

    if (ext == ".html" || ext == ".htm") {
        return "text/html";
    }
    if (ext == ".js") return "application/javascript";
    if (ext == ".json") return "application/json";
    if (ext == ".css") return "text/css";
    if (ext == ".lua" || ext == ".txt") return "text/plain";
    if (ext == ".png") return "image/png";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".webp") return "image/webp";

    return "application/octet-stream";
}

} // namespace

HttpServer::~HttpServer() {
    stop();
}

bool HttpServer::start(
    const std::string& host,
    uint16_t port,
    const std::string& rootDirectory)
{
    if (m_running) return true;

    m_host = host;
    m_port = port;
    m_rootDir = rootDirectory;
    m_running = true;

    m_thread =
        std::thread(
            &HttpServer::listenLoop,
            this);

    std::cout
        << "[HttpServer] Asset streaming server listening on http://"
        << host << ":" << port
        << " root=" << m_rootDir.string()
        << std::endl;

    return true;
}

void HttpServer::stop() {
    const bool wasRunning =
        m_running.exchange(false);

    if (!wasRunning) {
        if (m_thread.joinable()) {
            m_thread.join();
        }
        return;
    }

    if (m_thread.joinable()) {
        m_thread.join();
    }
}

void HttpServer::listenLoop() {
#ifdef _WIN32
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::cerr
            << "[HttpServer] WSAStartup failed."
            << std::endl;
        return;
    }
#endif

    SOCKET serverSock =
        socket(
            AF_INET,
            SOCK_STREAM,
            IPPROTO_TCP);

    if (serverSock == INVALID_SOCKET) {
        std::cerr
            << "[HttpServer] Failed to create socket."
            << std::endl;
#ifdef _WIN32
        WSACleanup();
#endif
        return;
    }

    int reuse = 1;
    setsockopt(
        serverSock,
        SOL_SOCKET,
        SO_REUSEADDR,
        reinterpret_cast<const char*>(&reuse),
        sizeof(reuse));

    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(m_port);
    serverAddr.sin_addr.s_addr = INADDR_ANY;

    if (bind(
            serverSock,
            reinterpret_cast<sockaddr*>(&serverAddr),
            sizeof(serverAddr)) == SOCKET_ERROR) {

        std::cerr
            << "[HttpServer] Failed to bind port "
            << m_port
            << std::endl;
        closesocket(serverSock);
#ifdef _WIN32
        WSACleanup();
#endif
        return;
    }

    if (listen(serverSock, 16) == SOCKET_ERROR) {
        std::cerr
            << "[HttpServer] listen() failed."
            << std::endl;
        closesocket(serverSock);
#ifdef _WIN32
        WSACleanup();
#endif
        return;
    }

    while (m_running) {
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(serverSock, &readSet);

        timeval tv{};
        tv.tv_sec = 0;
        tv.tv_usec = 250000;

        const int selected =
            select(
                0,
                &readSet,
                nullptr,
                nullptr,
                &tv);

        if (!m_running) break;
        if (selected <= 0) continue;

        sockaddr_in clientAddr{};
        int clientLen = sizeof(clientAddr);

        SOCKET clientSock =
            accept(
                serverSock,
                reinterpret_cast<sockaddr*>(&clientAddr),
                &clientLen);

        if (clientSock == INVALID_SOCKET) {
            continue;
        }

        std::string request;
        char buffer[8192];

        for (;;) {
            const int received =
                recv(
                    clientSock,
                    buffer,
                    sizeof(buffer),
                    0);

            if (received <= 0) break;

            request.append(buffer, buffer + received);

            if (request.find("\r\n\r\n") != std::string::npos ||
                request.size() > 1024 * 1024) {
                break;
            }
        }

        std::string response;

        const size_t lineEnd =
            request.find("\r\n");

        if (lineEnd == std::string::npos) {
            response = makeResponse(
                400,
                "Bad Request",
                "text/plain",
                "invalid http request");
        } else {
            const std::string requestLine =
                request.substr(0, lineEnd);

            std::istringstream lineStream(requestLine);
            std::string method;
            std::string target;
            std::string httpVersion;

            lineStream
                >> method
                >> target
                >> httpVersion;

            if (method != "GET") {
                response = makeResponse(
                    405,
                    "Method Not Allowed",
                    "text/plain",
                    "GET only");
            } else if (target == "/status") {
                response = makeResponse(
                    200,
                    "OK",
                    "application/json",
                    "{\"status\":\"online\",\"server\":\"FrontierMP\"}");
            } else if (target == "/manifest") {
                const std::string manifest =
                    buildManifest(m_rootDir);

                std::cout
                    << "[HttpServer] Served /manifest ("
                    << manifest.size()
                    << " bytes)."
                    << std::endl;

                response = makeResponse(
                    200,
                    "OK",
                    "text/plain; charset=utf-8",
                    manifest);
            } else if (
                target.rfind("/resources/", 0) == 0) {

                std::string relative =
                    target.substr(
                        std::string("/resources/").size());

                const size_t query =
                    relative.find('?');

                if (query != std::string::npos) {
                    relative.resize(query);
                }

                relative =
                    urlDecode(relative);

                const auto path =
                    m_rootDir / std::filesystem::path(relative);

                if (!isSafeRelativePath(
                        m_rootDir,
                        path)) {

                    response = makeResponse(
                        403,
                        "Forbidden",
                        "text/plain",
                        "invalid resource path");
                } else if (!std::filesystem::is_regular_file(path)) {
                    response = makeResponse(
                        404,
                        "Not Found",
                        "text/plain",
                        "resource file not found");
                } else {
                    std::ifstream file(
                        path,
                        std::ios::binary);

                    std::ostringstream contents;
                    contents << file.rdbuf();

                    response = makeResponse(
                        200,
                        "OK",
                        guessContentType(path),
                        contents.str());

                    std::cout
                        << "[HttpServer] Served resource "
                        << relative
                        << std::endl;
                }
            } else {
                response = makeResponse(
                    404,
                    "Not Found",
                    "text/plain",
                    "FrontierMP endpoint not found");
            }
        }

        size_t sent = 0;
        while (sent < response.size()) {
            const int result =
                send(
                    clientSock,
                    response.data() + sent,
                    static_cast<int>(
                        std::min<size_t>(
                            response.size() - sent,
                            64 * 1024)),
                    0);

            if (result <= 0) break;
            sent += static_cast<size_t>(result);
        }

        closesocket(clientSock);
    }

    closesocket(serverSock);
#ifdef _WIN32
    WSACleanup();
#endif
}

} // namespace Frontier::Server
