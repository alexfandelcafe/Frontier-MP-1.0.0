#pragma once

#include <string>
#include <functional>
#include <vector>

namespace Frontier::UI {

using JsCallback = std::function<void(const std::vector<std::string>& args)>;

class CefManager {
public:
    static CefManager& get();

    bool initialize();
    void shutdown();

    void createMainMenuBrowser(const std::string& url);
    void createChatBrowser(const std::string& url);

    void setChatOpen(bool open);
    bool isChatOpen() const { return m_chatOpen; }

    void setMainMenuVisible(bool visible);
    bool isMainMenuVisible() const { return m_mainMenuVisible; }

    // Enviar comandos/eventos hacia el navegador Web (JS)
    void executeJavaScript(const std::string& code);
    void sendChatMessageToUi(const std::string& message);

    // Bucle de ticks de CEF (CefDoMessageLoopWork)
    void update();

    // Procedimiento de ventana para capturar ratón y teclado
    bool handleWndProc(void* hwnd, uint32_t msg, uintptr_t wParam, intptr_t lParam);

private:
    CefManager() = default;
    ~CefManager() = default;

    bool m_initialized{false};
    bool m_chatOpen{false};
    bool m_mainMenuVisible{true};
};

} // namespace Frontier::UI
