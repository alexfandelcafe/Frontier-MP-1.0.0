#pragma once

#include <filesystem>
#include <cstdint>
#include <string>

namespace Frontier::Net {

class ResourceClient {
public:
    static bool httpGetStatus(
        const std::string& host,
        uint16_t httpPort);

    static bool downloadAll(
        const std::string& host,
        uint16_t httpPort,
        const std::filesystem::path& cacheRoot);

    // Equivalent client-side gate for ResourcesManager::LoadAllResources():
    // validate every downloaded resource tree before allowing InitSpawn/Welcome.
    static bool loadAllResources(
        const std::filesystem::path& cacheRoot,
        uint32_t& resourceCount,
        uint32_t& fileCount);

private:
    static bool httpGet(
        const std::string& host,
        uint16_t port,
        const std::string& path,
        std::string& body);

    static bool httpDownloadFile(
        const std::string& host,
        uint16_t port,
        const std::string& path,
        const std::filesystem::path& destination);
};

} // namespace Frontier::Net
