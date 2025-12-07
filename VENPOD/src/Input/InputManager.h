#pragma once

// =============================================================================
// VENPOD Input Manager - SDL3 Input Handling
// =============================================================================

#include <SDL3/SDL.h>
#include <glm/glm.hpp>
#include <array>
#include <cstdint>

namespace VENPOD::Input {

// Mouse button state
enum class MouseButton : uint8_t {
    Left = 0,
    Middle = 1,
    Right = 2,
    Count = 3
};

// Key codes for common operations
enum class KeyAction : uint8_t {
    CameraForward,
    CameraBackward,
    CameraLeft,
    CameraRight,
    CameraUp,
    CameraDown,
    MaterialNext,
    MaterialPrev,
    BrushIncrease,
    BrushDecrease,
    TogglePause,
    ResetWorld,
    Count
};

class InputManager {
public:
    InputManager() = default;
    ~InputManager() = default;

    // Non-copyable
    InputManager(const InputManager&) = delete;
    InputManager& operator=(const InputManager&) = delete;

    // Initialize with window dimensions and SDL window pointer
    void Initialize(uint32_t windowWidth, uint32_t windowHeight, SDL_Window* window = nullptr);

    // Set mouse capture mode (for FPS camera)
    void SetMouseCaptured(bool captured);
    bool IsMouseCaptured() const { return m_mouseCaptured; }

    // Process SDL events - call for each event in main loop
    void ProcessEvent(const SDL_Event& event);

    // Call at start of frame to update states
    void BeginFrame();

    // Call at end of frame to record previous states
    void EndFrame();

    // Update window dimensions on resize
    void OnResize(uint32_t width, uint32_t height);

    // Mouse state queries
    bool IsMouseButtonDown(MouseButton button) const;
    bool IsMouseButtonPressed(MouseButton button) const;  // Just pressed this frame
    bool IsMouseButtonReleased(MouseButton button) const; // Just released this frame

    glm::vec2 GetMousePosition() const { return m_mousePosition; }
    glm::vec2 GetMouseDelta() const { return m_mouseDelta; }
    glm::vec2 GetNormalizedMousePosition() const;  // 0-1 range
    float GetScrollDelta() const { return m_scrollDelta; }

    // Keyboard state queries
    bool IsKeyDown(SDL_Keycode key) const;
    bool IsKeyPressed(SDL_Keycode key) const;

    // Action queries (mapped from keys)
    bool IsActionDown(KeyAction action) const;
    bool IsActionPressed(KeyAction action) const;

    // Get mouse ray in normalized device coordinates (-1 to 1)
    glm::vec2 GetMouseNDC() const;

private:
    void UpdateKeyMappings();
    SDL_Keycode GetKeyForAction(KeyAction action) const;

    // Window dimensions and reference
    uint32_t m_windowWidth = 1920;
    uint32_t m_windowHeight = 1080;
    SDL_Window* m_window = nullptr;

    // Mouse state
    glm::vec2 m_mousePosition{0.0f};
    glm::vec2 m_lastMousePosition{0.0f};
    glm::vec2 m_mouseDelta{0.0f};
    float m_scrollDelta = 0.0f;
    bool m_mouseCaptured = false;

    std::array<bool, static_cast<size_t>(MouseButton::Count)> m_mouseButtonsDown{};
    std::array<bool, static_cast<size_t>(MouseButton::Count)> m_mouseButtonsPrev{};

    // Keyboard state (SDL manages this, we just query)
    const bool* m_keyboardState = nullptr;
    int m_numKeys = 0;

    // Track key states
    std::array<bool, 512> m_keysJustPressed{};  // True only on the frame the key was pressed
    std::array<bool, 512> m_keysDown{};         // True while key is held down

    // Key mappings
    std::array<SDL_Keycode, static_cast<size_t>(KeyAction::Count)> m_keyMappings{};
};

} // namespace VENPOD::Input
