#include "InputManager.h"
#include <spdlog/spdlog.h>
#include <algorithm>

namespace VENPOD::Input {

void InputManager::Initialize(uint32_t windowWidth, uint32_t windowHeight, SDL_Window* window) {
    m_windowWidth = windowWidth;
    m_windowHeight = windowHeight;
    m_window = window;

    // Initialize key mappings
    UpdateKeyMappings();

    // Get keyboard state pointer
    m_keyboardState = SDL_GetKeyboardState(&m_numKeys);

    // Initialize arrays
    m_mouseButtonsDown.fill(false);
    m_mouseButtonsPrev.fill(false);
    m_keysJustPressed.fill(false);
    m_keysDown.fill(false);

    spdlog::info("InputManager initialized: {}x{}", windowWidth, windowHeight);
}

void InputManager::SetMouseCaptured(bool captured) {
    if (!m_window) {
        spdlog::error("InputManager: No window set, cannot capture mouse");
        return;
    }

    if (captured) {
        // Raise window to bring it to front
        SDL_RaiseWindow(m_window);

        // CRITICAL: Hide cursor FIRST before enabling relative mode
        SDL_HideCursor();

        // Enable relative mouse mode (provides infinite movement and mouse delta)
        if (!SDL_SetWindowRelativeMouseMode(m_window, true)) {
            spdlog::error("Failed to enable relative mouse mode: {}", SDL_GetError());
            SDL_ShowCursor();  // Show cursor again if failed
            m_mouseCaptured = false;
            return;
        }

        m_mouseCaptured = true;
        spdlog::info("Mouse captured - FPS camera mode enabled (cursor hidden)");
    } else {
        // Disable relative mouse mode
        SDL_SetWindowRelativeMouseMode(m_window, false);

        // CRITICAL: Show cursor when releasing mouse
        SDL_ShowCursor();

        m_mouseCaptured = false;
        spdlog::info("Mouse released - cursor visible");
    }
}

void InputManager::UpdateKeyMappings() {
    // Default key mappings
    m_keyMappings[static_cast<size_t>(KeyAction::CameraForward)] = SDLK_W;
    m_keyMappings[static_cast<size_t>(KeyAction::CameraBackward)] = SDLK_S;
    m_keyMappings[static_cast<size_t>(KeyAction::CameraLeft)] = SDLK_A;
    m_keyMappings[static_cast<size_t>(KeyAction::CameraRight)] = SDLK_D;

    m_keyMappings[static_cast<size_t>(KeyAction::CameraUp)] = SDLK_SPACE;
    m_keyMappings[static_cast<size_t>(KeyAction::CameraDown)] = SDLK_LSHIFT;
    m_keyMappings[static_cast<size_t>(KeyAction::MaterialNext)] = SDLK_E;
    m_keyMappings[static_cast<size_t>(KeyAction::MaterialPrev)] = SDLK_Q;
    m_keyMappings[static_cast<size_t>(KeyAction::BrushIncrease)] = SDLK_RIGHTBRACKET;
    m_keyMappings[static_cast<size_t>(KeyAction::BrushDecrease)] = SDLK_LEFTBRACKET;
    m_keyMappings[static_cast<size_t>(KeyAction::TogglePause)] = SDLK_P;
    m_keyMappings[static_cast<size_t>(KeyAction::ResetWorld)] = SDLK_R;
}

void InputManager::ProcessEvent(const SDL_Event& event) {
    switch (event.type) {
        case SDL_EVENT_MOUSE_MOTION:
            if (m_mouseCaptured) {
                // In relative mode, use xrel/yrel for delta directly
                m_mouseDelta.x += event.motion.xrel;
                m_mouseDelta.y += event.motion.yrel;
            } else {
                // Normal mode - track absolute position
                m_mousePosition.x = event.motion.x;
                m_mousePosition.y = event.motion.y;
            }
            break;

        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            if (event.button.button == SDL_BUTTON_LEFT) {
                m_mouseButtonsDown[static_cast<size_t>(MouseButton::Left)] = true;
            } else if (event.button.button == SDL_BUTTON_MIDDLE) {
                m_mouseButtonsDown[static_cast<size_t>(MouseButton::Middle)] = true;
            } else if (event.button.button == SDL_BUTTON_RIGHT) {
                m_mouseButtonsDown[static_cast<size_t>(MouseButton::Right)] = true;
            }
            break;

        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (event.button.button == SDL_BUTTON_LEFT) {
                m_mouseButtonsDown[static_cast<size_t>(MouseButton::Left)] = false;
            } else if (event.button.button == SDL_BUTTON_MIDDLE) {
                m_mouseButtonsDown[static_cast<size_t>(MouseButton::Middle)] = false;
            } else if (event.button.button == SDL_BUTTON_RIGHT) {
                m_mouseButtonsDown[static_cast<size_t>(MouseButton::Right)] = false;
            }
            break;

        case SDL_EVENT_MOUSE_WHEEL:
            m_scrollDelta += event.wheel.y;
            break;

        case SDL_EVENT_KEY_DOWN:
            if (!event.key.repeat) {
                // SDL3 uses SDL_Keycode directly in key field
                SDL_Scancode scancode = SDL_GetScancodeFromKey(event.key.key, nullptr);
                if (scancode < 512) {
                    m_keysJustPressed[scancode] = true;
                    m_keysDown[scancode] = true;
                }
            }
            break;

        case SDL_EVENT_KEY_UP: {
            SDL_Scancode scancode = SDL_GetScancodeFromKey(event.key.key, nullptr);
            if (scancode < 512) {
                m_keysDown[scancode] = false;
            }
            break;
        }

        default:
            break;
    }
}

void InputManager::BeginFrame() {
    if (!m_mouseCaptured) {
        // In normal mode, calculate delta from position difference
        m_mouseDelta = m_mousePosition - m_lastMousePosition;
    }
    // In captured mode, delta is accumulated from motion events

    // Reset scroll delta at start of frame
    m_scrollDelta = 0.0f;
}

void InputManager::EndFrame() {
    // Store previous states for next frame
    m_lastMousePosition = m_mousePosition;
    m_mouseButtonsPrev = m_mouseButtonsDown;

    // In captured mode, reset delta after it's been used
    if (m_mouseCaptured) {
        m_mouseDelta = glm::vec2(0.0f);
    }

    // Clear "just pressed" flags at end of frame
    m_keysJustPressed.fill(false);
}

void InputManager::OnResize(uint32_t width, uint32_t height) {
    m_windowWidth = width;
    m_windowHeight = height;
}

bool InputManager::IsMouseButtonDown(MouseButton button) const {
    return m_mouseButtonsDown[static_cast<size_t>(button)];
}

bool InputManager::IsMouseButtonPressed(MouseButton button) const {
    size_t idx = static_cast<size_t>(button);
    return m_mouseButtonsDown[idx] && !m_mouseButtonsPrev[idx];
}

bool InputManager::IsMouseButtonReleased(MouseButton button) const {
    size_t idx = static_cast<size_t>(button);
    return !m_mouseButtonsDown[idx] && m_mouseButtonsPrev[idx];
}

glm::vec2 InputManager::GetNormalizedMousePosition() const {
    return glm::vec2(
        m_mousePosition.x / static_cast<float>(m_windowWidth),
        m_mousePosition.y / static_cast<float>(m_windowHeight)
    );
}

glm::vec2 InputManager::GetMouseNDC() const {
    glm::vec2 normalized = GetNormalizedMousePosition();
    return glm::vec2(
        normalized.x * 2.0f - 1.0f,
        1.0f - normalized.y * 2.0f  // Flip Y for NDC
    );
}

bool InputManager::IsKeyDown(SDL_Keycode key) const {
    SDL_Scancode scancode = SDL_GetScancodeFromKey(key, nullptr);
    if (scancode < 512) {
        return m_keysDown[scancode];
    }
    return false;
}

bool InputManager::IsKeyPressed(SDL_Keycode key) const {
    SDL_Scancode scancode = SDL_GetScancodeFromKey(key, nullptr);
    if (scancode < 512) {
        return m_keysJustPressed[scancode];
    }
    return false;
}

SDL_Keycode InputManager::GetKeyForAction(KeyAction action) const {
    return m_keyMappings[static_cast<size_t>(action)];
}

bool InputManager::IsActionDown(KeyAction action) const {
    return IsKeyDown(GetKeyForAction(action));
}

bool InputManager::IsActionPressed(KeyAction action) const {
    return IsKeyPressed(GetKeyForAction(action));
}

} // namespace VENPOD::Input
