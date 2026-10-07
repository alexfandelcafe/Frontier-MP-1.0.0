#include "server/server.hpp"
#include <iostream>
#include <csignal>

Frontier::Server::Server* g_serverInstance = nullptr;

void signalHandler(int signal) {
    if (g_serverInstance) {
        std::cout << "\n[Signal] Signal " << signal << " received. Exiting gracefully..." << std::endl;
        g_serverInstance->stop();
    }
}

int main(int argc, char* argv[]) {
    std::string configFile = "config.toml";
    if (argc > 1) {
        configFile = argv[1];
    }

    Frontier::Server::Server server;
    g_serverInstance = &server;

    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    if (!server.initialize(configFile)) {
        std::cerr << "[Main] Failed to start FrontierMP Server." << std::endl;
        return 1;
    }

    server.run();
    std::cout << "[Main] Server terminated cleanly." << std::endl;
    return 0;
}
