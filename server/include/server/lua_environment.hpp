#pragma once

#include "shared/types.hpp"
#include <string>
#include <vector>
#include <functional>
#include <unordered_map>
#include <memory>

namespace Frontier::Server {

class Server;

// Tipo genérico para argumentos de eventos en el script
using ScriptEventArgs = std::vector<std::string>;
using EventHandler = std::function<void(PlayerId source, const ScriptEventArgs& args)>;

class LuaEnvironment {
public:
    explicit LuaEnvironment(Server& server);
    ~LuaEnvironment();

    bool initialize();
    bool executeScript(const std::string& resourceName, const std::string& scriptPath);
    void reset();

    // Event system (FiveM style)
    void registerEvent(const std::string& eventName, EventHandler handler);
    void triggerEvent(const std::string& eventName, PlayerId source, const ScriptEventArgs& args);

    // Callbacks invokables desde C++
    void onPlayerJoin(PlayerId id, const std::string& name);
    void onPlayerLeave(PlayerId id, const std::string& name);
    void onPlayerChat(PlayerId id, const std::string& message);

private:
    void registerBindings();

    Server& m_server;
    std::unordered_map<std::string, std::vector<EventHandler>> m_eventHandlers;
};

} // namespace Frontier::Server
