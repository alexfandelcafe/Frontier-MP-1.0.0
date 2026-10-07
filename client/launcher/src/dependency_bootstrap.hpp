#pragma once

#include <filesystem>
#include <string>

namespace Frontier::Launcher {

struct DependencyBootstrapResult {
    bool ready{false};
    bool downloaded{false};
    bool installedToGame{false};
    std::filesystem::path scriptHookPath;
    std::filesystem::path loaderPath;
    std::string message;
};

DependencyBootstrapResult ensureScriptHookRDR(
    const std::filesystem::path& gameDirectory,
    const std::filesystem::path& launcherDirectory);

} // namespace Frontier::Launcher
