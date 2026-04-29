#pragma once

#include <Windows.h>

enum class LaunchMode {
    None,
    SandSimulator,
    Sandbox
};

// Show launcher dialog and return selected mode
LaunchMode ShowLauncherDialog(HINSTANCE hInstance);
