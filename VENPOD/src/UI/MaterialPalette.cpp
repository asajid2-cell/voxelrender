#include "MaterialPalette.h"
#include <imgui.h>
#include <spdlog/spdlog.h>

namespace VENPOD::UI {

void MaterialPalette::Initialize() {
    spdlog::info("MaterialPalette initialized");
}

bool MaterialPalette::Render(uint32_t& currentMaterial) {
    bool materialChanged = false;

    if (!m_windowOpen) {
        return false;
    }

    // Create a window for the material palette
    ImGui::Begin("Material Palette", &m_windowOpen, ImGuiWindowFlags_AlwaysAutoResize);

    // Display current selection at the top
    const MaterialInfo& currentInfo = s_materials[currentMaterial];
    ImGui::Text("Selected: %s (ID: %u)", currentInfo.name, currentInfo.id);
    ImGui::Separator();

    // Material grid - 4 buttons per row for nice layout
    const int buttonsPerRow = 4;
    const ImVec2 buttonSize(80, 60);

    for (size_t i = 0; i < s_materials.size(); ++i) {
        const MaterialInfo& mat = s_materials[i];

        // Push unique ID for this button
        ImGui::PushID(static_cast<int>(i));

        // Highlight selected material with a colored border
        if (mat.id == currentMaterial) {
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.0f, 1.0f, 0.0f, 1.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 3.0f);
        }

        // Create colored button
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(mat.colorR, mat.colorG, mat.colorB, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(
            mat.colorR * 1.2f,
            mat.colorG * 1.2f,
            mat.colorB * 1.2f,
            1.0f
        ));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(
            mat.colorR * 0.8f,
            mat.colorG * 0.8f,
            mat.colorB * 0.8f,
            1.0f
        ));

        // Render button with material name
        if (ImGui::Button(mat.name, buttonSize)) {
            if (currentMaterial != mat.id) {
                currentMaterial = mat.id;
                materialChanged = true;
                spdlog::info("Material changed to: {} (ID: {})", mat.name, mat.id);
            }
        }

        // Restore colors
        ImGui::PopStyleColor(3);
        if (mat.id == currentMaterial) {
            ImGui::PopStyleColor();
            ImGui::PopStyleVar();
        }

        // Show tooltip on hover
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::Text("%s", mat.description);
            ImGui::EndTooltip();
        }

        ImGui::PopID();

        // New row after every N buttons
        if ((i + 1) % buttonsPerRow != 0 && i != s_materials.size() - 1) {
            ImGui::SameLine();
        }
    }

    ImGui::End();

    return materialChanged;
}

} // namespace VENPOD::UI
