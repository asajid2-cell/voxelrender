// =============================================================================
// VENPOD Launcher - Mode Selection
// =============================================================================

#include <Windows.h>
#include <string>
#include <fstream>

// Simple file logging for debugging
static void LauncherLog(const char* msg) {
    std::ofstream log("venpod_startup.log", std::ios::app);
    if (log.is_open()) {
        log << "[Launcher] " << msg << std::endl;
        log.flush();
    }
}

enum class LaunchMode {
    None,
    SandSimulator,
    Sandbox
};

// Global variable to store selected mode
static LaunchMode g_selectedMode = LaunchMode::None;
// Flag to exit message loop without PostQuitMessage (which pollutes SDL's event queue)
static bool g_dialogClosed = false;

// Window procedure for the launcher window
LRESULT CALLBACK LauncherWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_COMMAND:
            {
                int wmId = LOWORD(wParam);
                if (wmId == 101) {
                    g_selectedMode = LaunchMode::SandSimulator;
                    g_dialogClosed = true;
                    DestroyWindow(hwnd);
                    return 0;
                } else if (wmId == 102) {
                    g_selectedMode = LaunchMode::Sandbox;
                    g_dialogClosed = true;
                    DestroyWindow(hwnd);
                    return 0;
                }
            }
            break;

        case WM_CLOSE:
            g_selectedMode = LaunchMode::None;
            g_dialogClosed = true;
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            // DO NOT use PostQuitMessage here!
            // It pollutes the thread message queue and SDL3 will see it as SDL_EVENT_QUIT
            // causing the game to exit immediately after the launcher closes.
            // Instead, we use g_dialogClosed flag to exit our message loop.
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// Create and show launcher dialog
LaunchMode ShowLauncherDialog(HINSTANCE hInstance) {
    LauncherLog("ShowLauncherDialog entered");

    // Register window class
    const wchar_t CLASS_NAME[] = L"VENPODLauncherClass";

    WNDCLASSW wc = {};
    wc.lpfnWndProc = LauncherWndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);

    LauncherLog("Registering window class...");
    ATOM classAtom = RegisterClassW(&wc);
    if (!classAtom) {
        LauncherLog("ERROR: RegisterClassW failed!");
        MessageBoxW(NULL, L"Failed to register window class!", L"VENPOD Launcher Error", MB_OK | MB_ICONERROR);
        return LaunchMode::Sandbox;  // Default to sandbox
    }
    LauncherLog("Window class registered successfully");

    // Create launcher window
    LauncherLog("Creating launcher window...");
    HWND hwnd = CreateWindowExW(
        WS_EX_DLGMODALFRAME | WS_EX_TOPMOST,
        CLASS_NAME,
        L"VENPOD Launcher",
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, 400, 250,
        NULL, NULL, hInstance, NULL
    );

    if (!hwnd) {
        DWORD err = GetLastError();
        LauncherLog("ERROR: CreateWindowExW failed!");
        wchar_t msg[256];
        swprintf_s(msg, L"Failed to create launcher window! Error: %lu", err);
        MessageBoxW(NULL, msg, L"VENPOD Launcher Error", MB_OK | MB_ICONERROR);
        return LaunchMode::Sandbox;  // Default to sandbox if window creation fails
    }
    LauncherLog("Launcher window created successfully");

    // Center the window
    RECT rc;
    GetWindowRect(hwnd, &rc);
    int xPos = (GetSystemMetrics(SM_CXSCREEN) - (rc.right - rc.left)) / 2;
    int yPos = (GetSystemMetrics(SM_CYSCREEN) - (rc.bottom - rc.top)) / 2;
    SetWindowPos(hwnd, NULL, xPos, yPos, 0, 0, SWP_NOSIZE);

    // Create title
    CreateWindowW(L"STATIC", L"VENPOD - Voxel Physics Engine",
        WS_CHILD | WS_VISIBLE | SS_CENTER,
        20, 20, 360, 30,
        hwnd, NULL, hInstance, NULL);

    // Create subtitle
    CreateWindowW(L"STATIC", L"Select a mode to launch:",
        WS_CHILD | WS_VISIBLE | SS_CENTER,
        20, 60, 360, 20,
        hwnd, NULL, hInstance, NULL);

    // Create Sand Simulator button
    HWND btnSandSim = CreateWindowW(L"BUTTON", L"Sand Simulator\n(Material Physics & Gravity)",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_MULTILINE | BS_CENTER,
        50, 100, 140, 80,
        hwnd, (HMENU)101, hInstance, NULL);

    // Create Sandbox button
    HWND btnSandbox = CreateWindowW(L"BUTTON", L"Sandbox Mode\n(Infinite Terrain Explorer)",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_MULTILINE | BS_CENTER,
        210, 100, 140, 80,
        hwnd, (HMENU)102, hInstance, NULL);

    // Set default font
    HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    SendMessage(btnSandSim, WM_SETFONT, (WPARAM)hFont, TRUE);
    SendMessage(btnSandbox, WM_SETFONT, (WPARAM)hFont, TRUE);

    // Show the window
    LauncherLog("Showing window...");
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    LauncherLog("Window shown, entering message loop...");

    // Reset dialog closed flag
    g_dialogClosed = false;

    // Message loop - uses flag instead of PostQuitMessage to avoid polluting SDL's event queue
    MSG msg;
    int msgCount = 0;
    while (!g_dialogClosed) {
        // Use PeekMessage + WaitMessage instead of GetMessage to avoid blocking on WM_QUIT
        if (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            msgCount++;
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        } else {
            // No messages pending, wait for one
            WaitMessage();
        }
    }

    char logBuf[256];
    sprintf_s(logBuf, "Message loop exited after %d messages, g_selectedMode = %d", msgCount, (int)g_selectedMode);
    LauncherLog(logBuf);

    // Unregister window class
    UnregisterClassW(CLASS_NAME, hInstance);

    LauncherLog("ShowLauncherDialog exiting");
    return g_selectedMode;
}
