#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <filesystem>
#include <memory>

namespace Frontier::Server {

struct ResourceInfo {
    std::string name;
    std::string path;
    std::string description;
    std::string version;
    std::string author;

    std::vector<std::string> serverScripts;
    std::vector<std::string> clientScripts;
    std::vector<std::string> files; // Archivos de UI, texturas, audios
    bool isRunning{false};
};

class Server; // Forward declaration

class ResourceManager {
public:
    explicit ResourceManager(Server& server);
    ~ResourceManager() = default;

    void discoverResources(const std::string& resourcesDirectory);
    bool startResource(const std::string& name);
    bool stopResource(const std::string& name);
    bool restartResource(const std::string& name);

    const ResourceInfo* getResource(const std::string& name) const;
    const std::unordered_map<std::string, ResourceInfo>& getAllResources() const { return m_resources; }

    std::vector<std::string> getRunningResources() const;

private:
    bool parseManifest(const std::filesystem::path& manifestPath, ResourceInfo& outInfo);

    Server& m_server;
    std::string m_resourcesPath;
    std::unordered_map<std::string, ResourceInfo> m_resources;
};

} // namespace Frontier::Server
