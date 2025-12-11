// =============================================================================
// VENPOD - High-Performance Voxel Physics Engine
// Main Entry Point with Mode Launcher
// =============================================================================

#include "launcher.h"
#include <Windows.h>

// Forward declarations for mode entry points
int RunSandSimulator(int argc, char* argv[]);
int RunSandbox(int argc, char* argv[]);

#ifdef _WIN32
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    (void)hPrevInstance;
    (void)lpCmdLine;
    (void)nCmdShow;

    // Show launcher dialog
    LaunchMode mode = ShowLauncherDialog(hInstance);

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
