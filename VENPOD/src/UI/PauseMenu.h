#pragma once

// =============================================================================
// VENPOD Pause Menu - Main UI control panel
// =============================================================================

#include "../Input/BrushController.h"

namespace VENPOD::UI {

class MaterialPalette;
class BrushPanel;

class PauseMenu {
public:
    PauseMenu() = default;
    ~PauseMenu() = default;

    // Non-copyable
    PauseMenu(const PauseMenu&) = delete;
    PauseMenu& operator=(const PauseMenu&) = delete;

    void Initialize();

    // Toggle pause menu visibility
    void Toggle();
    void Show();
    void Hide();
    bool IsVisible() const { return m_visible; }

    // Render pause menu and control other UI panels
    void Render(
        bool& paused,
        uint64_t frameCount,
        const glm::vec3& cameraPos,
        MaterialPalette& materialPalette,
        BrushPanel& brushPanel,
        Input::BrushController& brushController
    );

private:
    bool m_visible = false;
    bool m_showMaterialPalette = true;
    bool m_showBrushPanel = true;
    bool m_showPerformance = true;
};

} // namespace VENPOD::UI