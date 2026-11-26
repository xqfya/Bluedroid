#include "emulator/app_setup.h"
#include "emulator/ui_window.h"
#include "emulator/config.h"
#include "emulator/logger.h"
#include <windows.h>
#include <filesystem>
#include <string>

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int nCmdShow) {
    // If BlueDroid is already in the tray, bring it back instead of starting a
    // second manager that would fight over the same phones/ports.
    if (HWND existing = FindWindowW(L"BlueDroidEmulatorWindow", nullptr)) {
        ShowWindow(existing, SW_SHOW);
        PostMessageW(existing, WM_APP + 2, 0, 0); // restore from tray
        SetForegroundWindow(existing);
        return 0;
    }

    // First-run bootstrap must happen before config and logger touch any paths.
    emu::AppSetup::ensure();

    auto& cfg = emu::ConfigManager::instance();
    cfg.load(emu::AppSetup::configPath());
    cfg.config().dataDir = emu::AppSetup::dataDir();

    std::error_code ec;
    for (const std::string& dir : {
             emu::AppSetup::dataDir(), emu::AppSetup::imagesDir(),
             emu::AppSetup::thumbsDir(), emu::AppSetup::backupsDir(),
             emu::AppSetup::runtimeDir(), emu::AppSetup::tempDir()}) {
        std::filesystem::create_directories(dir, ec);
    }

    emu::Logger::instance().setLevel(emu::LogLevel::Info);
    emu::Logger::instance().setFile(emu::AppSetup::logPath());

    emu::MainWindow win;
    if (!win.create(hInst, nCmdShow)) {
        MessageBoxW(nullptr, L"Failed to create window", L"Error", MB_OK);
        return 1;
    }
    win.runMessageLoop();
    return 0;
}

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    HINSTANCE hInst = GetModuleHandle(nullptr);
    return wWinMain(hInst, nullptr, GetCommandLineW(), SW_SHOWDEFAULT);
}
