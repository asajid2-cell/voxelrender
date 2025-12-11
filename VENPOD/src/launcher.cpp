// =============================================================================
// VENPOD Launcher - Mode Selection
// =============================================================================

#include <Windows.h>
#include <string>

enum class LaunchMode {
    None,
    SandSimulator,
    Sandbox
};

// Global variable to store selected mode
static LaunchMode g_selectedMode = LaunchMode::None;

// Window procedure for the launcher window
LRESULT CALLBACK LauncherWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_COMMAND:
            {
                int wmId = LOWORD(wParam);
                if (wmId == 101) {
                    g_selectedMode = LaunchMode::SandSimulator;
                    DestroyWindow(hwnd);
                    return 0;
                } else if (wmId == 102) {
                    g_selectedMode = LaunchMode::Sandbox;
                    DestroyWindow(hwnd);
                    return 0;
                }
            }
            break;

        case WM_CLOSE:
            g_selectedMode = LaunchMode::None;
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// Create and show launcher dialog
LaunchMode ShowLauncherDialog(HINSTANCE hInstance) {
    // Register window class
    const wchar_t CLASS_NAME[] = L"VENPODLauncherClass";

    WNDCLASSW wc = {};
    wc.lpfnWndProc = LauncherWndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);

    RegisterClassW(&wc);
    // Create launcher window
    HWND hwnd = CreateWindowExW(
        WS_EX_DLGMODALFRAME | WS_EX_TOPMOST,
        CLASS_NAME,
        L"VENPOD Launcher",
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, 400, 250,
        NULL, NULL, hInstance, NULL
    );

    if (!hwnd) {
        return LaunchMode::Sandbox;  // Default to sandbox if window creation fails
    }

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
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    // Message loop
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    // Unregister window class
    UnregisterClassW(CLASS_NAME, hInstance);

    return g_selectedMode;
}
