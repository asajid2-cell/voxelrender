#include "BrushPanel.h"
#include <imgui.h>
#include <spdlog/spdlog.h>

namespace VENPOD::UI {

void BrushPanel::Initialize() {
    spdlog::info("BrushPanel initialized");
}

const char* BrushPanel::BrushModeToString(Input::BrushMode mode) {
    switch (mode) {
        case Input::BrushMode::Paint:   return "Paint";
        case Input::BrushMode::Erase:   return "Erase";
        case Input::BrushMode::Replace: return "Replace";
        case Input::BrushMode::Fill:    return "Fill";
        default:                        return "Unknown";
    }
}

const char* BrushPanel::BrushShapeToString(Input::BrushShape shape) {
    switch (shape) {
        case Input::BrushShape::Sphere:   return "Sphere";
        case Input::BrushShape::Cube:     return "Cube";
        case Input::BrushShape::Cylinder: return "Cylinder";
        default:                          return "Unknown";
    }
}

void BrushPanel::Render(Input::BrushController& brush) {
    if (!m_windowOpen) {
        return;
    }

    ImGui::Begin("Brush Settings", &m_windowOpen, ImGuiWindowFlags_AlwaysAutoResize);

    // === BRUSH RADIUS ===
    ImGui::Text("Brush Radius");
    float radius = brush.GetRadius();
    if (ImGui::SliderFloat("##radius", &radius, 1.0f, 32.0f, "%.1f voxels")) {
        brush.SetRadius(radius);
    }

    // Quick buttons for common sizes
    ImGui::Text("Quick Size:");
    ImGui::SameLine();
    if (ImGui::Button("1")) brush.SetRadius(1.0f);
    ImGui::SameLine();
    if (ImGui::Button("3")) brush.SetRadius(3.0f);
    ImGui::SameLine();
    if (ImGui::Button("5")) brush.SetRadius(5.0f);
    ImGui::SameLine();
    if (ImGui::Button("10")) brush.SetRadius(10.0f);
    ImGui::SameLine();
    if (ImGui::Button("20")) brush.SetRadius(20.0f);

    ImGui::Separator();

    // === BRUSH MODE ===
    ImGui::Text("Brush Mode");

    Input::BrushMode currentMode = brush.GetMode();
    int modeIndex = static_cast<int>(currentMode);

    if (ImGui::RadioButton("Paint", modeIndex == 0)) {
        brush.SetMode(Input::BrushMode::Paint);
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("Erase", modeIndex == 1)) {
        brush.SetMode(Input::BrushMode::Erase);
    }

    if (ImGui::RadioButton("Replace", modeIndex == 2)) {
        brush.SetMode(Input::BrushMode::Replace);
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("Fill", modeIndex == 3)) {
        brush.SetMode(Input::BrushMode::Fill);
    }

    ImGui::Separator();

    // === BRUSH SHAPE ===
    ImGui::Text("Brush Shape");

    Input::BrushShape currentShape = brush.GetShape();
    int shapeIndex = static_cast<int>(currentShape);

    if (ImGui::RadioButton("Sphere", shapeIndex == 0)) {
        brush.SetShape(Input::BrushShape::Sphere);
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("Cube", shapeIndex == 1)) {
        brush.SetShape(Input::BrushShape::Cube);
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("Cylinder", shapeIndex == 2)) {
        brush.SetShape(Input::BrushShape::Cylinder);
    }

    ImGui::Separator();

    // === CURRENT STATUS ===
    ImGui::Text("Status:");
    ImGui::BulletText("Radius: %.1f voxels", brush.GetRadius());
    ImGui::BulletText("Mode: %s", BrushModeToString(brush.GetMode()));
    ImGui::BulletText("Shape: %s", BrushShapeToString(brush.GetShape()));
    ImGui::BulletText("Material: %u", brush.GetMaterial());

    if (brush.HasValidPosition()) {
        auto pos = brush.GetBrushPosition();
        ImGui::BulletText("Position: (%.1f, %.1f, %.1f)", pos.x, pos.y, pos.z);
    } else {
        ImGui::BulletText("Position: No target");
    }

    ImGui::Separator();

    // === CONTROLS HELP ===
    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Controls:");
    ImGui::BulletText("LMB: Paint");
    ImGui::BulletText("RMB: Erase");
    ImGui::BulletText("R/F: Adjust radius");
    ImGui::BulletText("Q/E: Change material");
    ImGui::BulletText("Scroll: Adjust radius");

    ImGui::End();
}

} // namespace VENPOD::UI
