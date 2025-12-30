// =============================================================================
// VENPOD - High-Performance Voxel Physics Engine
// Main Entry Point with Mode Launcher
// =============================================================================

#include "launcher.h"
#include <Windows.h>
#include <fstream>
#include <chrono>

// Simple file logging for debugging before spdlog is available
static void LogToFile(const char* msg) {
    std::ofstream log("venpod_startup.log", std::ios::app);
    if (log.is_open()) {
        log << msg << std::endl;
        log.flush();
    }
}

// Forward declarations for mode entry points
int RunSandSimulator(int argc, char* argv[]);
int RunSandbox(int argc, char* argv[]);

#ifdef _WIN32
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    (void)hPrevInstance;
    (void)lpCmdLine;
    (void)nCmdShow;

    LogToFile("=== WinMain started ===");

    // Show launcher dialog
    LogToFile("Calling ShowLauncherDialog...");
    LaunchMode mode = ShowLauncherDialog(hInstance);

    // Log what mode was returned
    const char* modeStr = "Unknown";
    switch (mode) {
        case LaunchMode::None: modeStr = "None"; break;
        case LaunchMode::SandSimulator: modeStr = "SandSimulator"; break;
        case LaunchMode::Sandbox: modeStr = "Sandbox"; break;
    }
    char logBuf[256];
    sprintf_s(logBuf, "ShowLauncherDialog returned: %s", modeStr);
    LogToFile(logBuf);

    // Launch selected mode
    int argc = 0;
    char** argv = nullptr;

    switch (mode) {
        case LaunchMode::SandSimulator:
            return RunSandSimulator(argc, argv);
        case LaunchMode::Sandbox:
            return RunSandbox(argc, argv);
        case LaunchMode::None:
        default:
            return 0;  // User canceled
    }
}
#else
// Fallback for non-Windows platforms - default to sandbox mode
int main(int argc, char* argv[]) {
    return RunSandbox(argc, argv);
}
#endif
