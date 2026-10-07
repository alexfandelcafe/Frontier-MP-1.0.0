#include "server/resource_manager.hpp"
#include "server/server.hpp"
#include <iostream>
#include <fstream>
#include <sstream>

namespace Frontier::Server {

ResourceManager::ResourceManager(Server& server)
    : m_server(server) {}

void ResourceManager::discoverResources(const std::string& resourcesDirectory) {
    m_resourcesPath = resourcesDirectory;
    m_resources.clear();

    namespace fs = std::filesystem;
    if (!fs::exists(resourcesDirectory)) {
        std::cerr << "[ResourceManager] Directory not found: " << resourcesDirectory << std::endl;
        return;
    }

    for (const auto& entry : fs::directory_iterator(resourcesDirectory)) {
        if (entry.is_directory()) {
            std::string resName = entry.path().filename().string();
            fs::path manifestPath = entry.path() / "manifest.toml";

            ResourceInfo info;
            info.name = resName;
            info.path = entry.path().string();

            if (fs::exists(manifestPath)) {
                if (parseManifest(manifestPath, info)) {
                    m_resources[resName] = info;
                    std::cout << "[ResourceManager] Discovered resource: " << resName 
                              << " (" << info.serverScripts.size() << " server scripts, "
                              << info.clientScripts.size() << " client scripts)" << std::endl;
                }
            } else {
                // Si no hay manifest, buscar server.lua y client.lua por defecto
                if (fs::exists(entry.path() / "server.lua")) info.serverScripts.push_back("server.lua");
                if (fs::exists(entry.path() / "client.lua")) info.clientScripts.push_back("client.lua");
                m_resources[resName] = info;
            }
        }
    }
}

bool ResourceManager::parseManifest(const std::filesystem::path& manifestPath, ResourceInfo& outInfo) {
    std::ifstream file(manifestPath);
    if (!file.is_open()) return false;

    std::string line;
    std::string currentArray;

    while (std::getline(file, line)) {
        // Trim whitespace
        size_t first = line.find_first_not_of(" \t\r\n");
        if (first == std::string::npos || line[first] == '#') continue;
        size_t last = line.find_last_not_of(" \t\r\n");
        line = line.substr(first, (last - first + 1));

        if (line.find("description =") != std::string::npos) {
            size_t q1 = line.find('\"');
            size_t q2 = line.rfind('\"');
            if (q1 != std::string::npos && q2 > q1) {
                outInfo.description = line.substr(q1 + 1, q2 - q1 - 1);
            }
        } else if (line.find("server_scripts") != std::string::npos) {
            currentArray = "server";
        } else if (line.find("client_scripts") != std::string::npos) {
            currentArray = "client";
        } else if (line.find("files") != std::string::npos) {
            currentArray = "files";
        }

        if (!currentArray.empty()) {
            size_t q1 = line.find('\"');
            size_t q2 = line.rfind('\"');
            if (q1 != std::string::npos && q2 > q1 && q1 != q2) {
                std::string item = line.substr(q1 + 1, q2 - q1 - 1);
                if (currentArray == "server") outInfo.serverScripts.push_back(item);
                else if (currentArray == "client") outInfo.clientScripts.push_back(item);
                else if (currentArray == "files") outInfo.files.push_back(item);
            }
            if (line.find(']') != std::string::npos) {
                currentArray.clear();
            }
        }
    }
    return true;
}

bool ResourceManager::startResource(const std::string& name) {
    auto it = m_resources.find(name);
    if (it == m_resources.end()) {
        std::cerr << "[ResourceManager] Could not start resource '" << name << "': not found" << std::endl;
        return false;
    }

    if (it->second.isRunning) {
        std::cout << "[ResourceManager] Resource '" << name << "' is already running." << std::endl;
        return true;
    }

    std::cout << "[ResourceManager] Starting resource '" << name << "'..." << std::endl;

    // Ejecutar scripts del lado servidor
    for (const auto& script : it->second.serverScripts) {
        std::filesystem::path scriptFullPath = std::filesystem::path(it->second.path) / script;
        if (!m_server.getLuaEnvironment().executeScript(name, scriptFullPath.string())) {
            std::cerr << "[ResourceManager] Failed to execute server script: " << script << std::endl;
            return false;
        }
    }

    it->second.isRunning = true;
    std::cout << "[ResourceManager] Resource '" << name << "' started successfully." << std::endl;
    return true;
}

bool ResourceManager::stopResource(const std::string& name) {
    auto it = m_resources.find(name);
    if (it == m_resources.end() || !it->second.isRunning) {
        return false;
    }

    std::cout << "[ResourceManager] Stopping resource '" << name << "'..." << std::endl;
    it->second.isRunning = false;
    return true;
}

bool ResourceManager::restartResource(const std::string& name) {
    stopResource(name);
    return startResource(name);
}

const ResourceInfo* ResourceManager::getResource(const std::string& name) const {
    auto it = m_resources.find(name);
    if (it != m_resources.end()) {
        return &it->second;
    }
    return nullptr;
}

std::vector<std::string> ResourceManager::getRunningResources() const {
    std::vector<std::string> running;
    for (const auto& [name, info] : m_resources) {
        if (info.isRunning) running.push_back(name);
    }
    return running;
}

} // namespace Frontier::Server
