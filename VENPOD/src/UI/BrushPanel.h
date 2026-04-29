#pragma once

// =============================================================================
// VENPOD Brush Panel - Brush settings UI (radius, mode, shape)
// =============================================================================

#include "../Input/BrushController.h"

namespace VENPOD::UI {

class BrushPanel {
public:
    BrushPanel() = default;
    ~BrushPanel() = default;

    // Non-copyable
    BrushPanel(const BrushPanel&) = delete;
    BrushPanel& operator=(const BrushPanel&) = delete;

    void Initialize();

    // Render brush settings panel
    // Modifies brush controller directly
    void Render(Input::BrushController& brush);

private:
    bool m_windowOpen = true;

    // Helper to convert enum to display string
    static const char* BrushModeToString(Input::BrushMode mode);
    static const char* BrushShapeToString(Input::BrushShape shape);
};

} // namespace VENPOD::UI
