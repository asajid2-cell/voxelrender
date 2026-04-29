#include "PauseMenu.h"
#include "MaterialPalette.h"
#include "BrushPanel.h"
#include <imgui.h>
#include <spdlog/spdlog.h>
#include <glm/glm.hpp>

namespace VENPOD::UI {

void PauseMenu::Initialize() {
    spdlog::info("PauseMenu initialized");
}

void PauseMenu::Toggle() {
    m_visible = !m_visible;
    spdlog::info("Pause menu {}", m_visible ? "opened" : "closed");
}

void PauseMenu::Show() {
    m_visible = true;
}

void PauseMenu::Hide() {
    m_visible = false;
}

void PauseMenu::Render(
    bool& paused,
    uint64_t frameCount,
    const glm::vec3& cameraPos,
    MaterialPalette& materialPalette,
    BrushPanel& brushPanel,
    Input::BrushController& brushController
) {
    if (!m_visible) {
        // When pause menu is hidden, hide all UI panels
        return;
    }

    // =============================================================================
    // Main Pause Menu Window (Center of screen)
    // =============================================================================
    ImGuiIO& io = ImGui::GetIO();
    ImVec2 center(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
    ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(400, 0), ImGuiCond_Always);

    ImGui::Begin("Pause Menu", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);

    ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "PAUSED");
    ImGui::Separator();

    // Simulation Control
    ImGui::Text("Simulation");
    if (ImGui::Checkbox("Pause Physics", &paused)) {
        spdlog::info("Simulation {}", paused ? "paused" : "resumed");
    }
    ImGui::Separator();

    // UI Panel Toggles
    ImGui::Text("UI Panels");
    ImGui::Checkbox("Show Material Palette", &m_showMaterialPalette);
    ImGui::Checkbox("Show Brush Settings", &m_showBrushPanel);
    ImGui::Checkbox("Show Performance", &m_showPerformance);
    ImGui::Separator();

    // Performance Stats
    ImGui::Text("Stats");
    ImGui::BulletText("FPS: %.1f (%.2f ms)", io.Framerate, 1000.0f / io.Framerate);
    ImGui::BulletText("Frame: %llu", frameCount);
    ImGui::BulletText("Camera: (%.1f, %.1f, %.1f)", cameraPos.x, cameraPos.y, cameraPos.z);
    ImGui::Separator();

    // Controls
    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Controls:");
    ImGui::BulletText("ESC: Toggle this menu");
    ImGui::BulletText("Tab: Toggle mouse capture");
    ImGui::BulletText("WASD: Move camera");
    ImGui::BulletText("Mouse: Look around");
    ImGui::BulletText("Space/Shift: Up/Down");
    ImGui::BulletText("LMB: Paint");
    ImGui::BulletText("RMB: Erase");
    ImGui::BulletText("Q/E: Change material");
    ImGui::BulletText("R/F: Change brush size");
    ImGui::BulletText("Scroll: Change brush size");
    ImGui::Separator();

    ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f), "Press ESC to resume");

    ImGui::End();

    // =============================================================================
    // Render UI Panels (Fixed positions, only when pause menu is open)
    // =============================================================================

    // Material Palette - Top Left
    if (m_showMaterialPalette) {
        ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
        uint32_t currentMaterial = brushController.GetMaterial();
        if (materialPalette.Render(currentMaterial)) {
            brushController.SetMaterial(currentMaterial);
        }
    }

    // Brush Panel - Below Material Palette
    if (m_showBrushPanel) {
        ImGui::SetNextWindowPos(ImVec2(10, 400), ImGuiCond_FirstUseEver);
        brushPanel.Render(brushController);
    }

    // Performance Overlay - Top Right
    if (m_showPerformance) {
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 310, 10), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(300, 0), ImGuiCond_Always);

        ImGui::Begin("Performance", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
        ImGui::Text("FPS: %.1f (%.2f ms)", io.Framerate, 1000.0f / io.Framerate);
        ImGui::Text("Frame: %llu", frameCount);
        ImGui::Separator();
        ImGui::Text("Camera: (%.1f, %.1f, %.1f)", cameraPos.x, cameraPos.y, cameraPos.z);
        ImGui::Separator();

        // Display current material info
        uint32_t matId = brushController.GetMaterial();
        static const char* materialNames[] = {
            "Air", "Sand", "Water", "Stone", "Dirt", "Wood", "Fire", "Lava",
            "Ice", "Oil", "Glass", "Smoke", "Acid", "Honey", "Concrete",
            "Gunpowder", "Crystal", "Steam"
        };
        const char* matName = (matId < 18) ? materialNames[matId] : "Unknown";
        ImGui::Text("Material: %u - %s", matId, matName);
        ImGui::Text("Brush Radius: %.1f", brushController.GetRadius());
        ImGui::End();
    }
}

} // namespace VENPOD::UI