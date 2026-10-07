#include "ui/cef_manager.hpp"
#include <iostream>

namespace Frontier::UI {

CefManager& CefManager::get() {
    static CefManager instance;
    return instance;
}

bool CefManager::initialize() {
    if (m_initialized) return true;

    std::cout << "[CEFManager] Initializing Chromium Embedded Framework..." << std::endl;
    // En una compilación con libcef.lib, se configuran CefSettings y CefInitialize.
    m_initialized = true;
    return true;
}

void CefManager::shutdown() {
    if (!m_initialized) return;
    std::cout << "[CEFManager] Shutting down CEF..." << std::endl;
    m_initialized = false;
}

void CefManager::createMainMenuBrowser(const std::string& url) {
    std::cout << "[CEFManager] Created Main Menu browser instance at: " << url << std::endl;
}

void CefManager::createChatBrowser(const std::string& url) {
    std::cout << "[CEFManager] Created Chat browser instance at: " << url << std::endl;
}

void CefManager::setChatOpen(bool open) {
    m_chatOpen = open;
    executeJavaScript(open ? "set_chatbox_open(true);" : "set_chatbox_open(false);");
}

void CefManager::setMainMenuVisible(bool visible) {
    m_mainMenuVisible = visible;
    executeJavaScript(visible ? "show_loading_screen(false); show_page('home');" : "$('#mainmenu').hide();");
}

void CefManager::executeJavaScript(const std::string& code) {
    // Envía código JS al frame principal del navegador CEF
}

void CefManager::sendChatMessageToUi(const std::string& message) {
    executeJavaScript("send_chat_message(\"" + message + "\");");
}

void CefManager::update() {
    if (!m_initialized) return;
    // CefDoMessageLoopWork();
}

bool CefManager::handleWndProc(void* hwnd, uint32_t msg, uintptr_t wParam, intptr_t lParam) {
    // Si el menú o el chat están abiertos, capturar el teclado/ratón y no pasar a RDR1
    if (m_chatOpen || m_mainMenuVisible) {
        // Enviar eventos a CefBrowserHost::SendKeyEvent o SendMouseMoveEvent
        return true; // Mensaje consumido por la interfaz
    }
    return false; // Pasar al juego
}

} // namespace Frontier::UI
