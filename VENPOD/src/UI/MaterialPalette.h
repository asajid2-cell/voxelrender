#pragma once

// =============================================================================
// VENPOD Material Palette - Visual material selector with colored buttons
// =============================================================================

#include <cstdint>
#include <array>

namespace VENPOD::UI {

class MaterialPalette {
public:
    MaterialPalette() = default;
    ~MaterialPalette() = default;

    // Non-copyable
    MaterialPalette(const MaterialPalette&) = delete;
    MaterialPalette& operator=(const MaterialPalette&) = delete;

    void Initialize();

    // Render the material palette window
    // Pass current material by reference - will be modified if user clicks a button
    // Returns true if material was changed this frame
    bool Render(uint32_t& currentMaterial);

private:
    struct MaterialInfo {
        uint32_t id;
        const char* name;
        float colorR, colorG, colorB;
        const char* description;  // Tooltip text
    };

    // All 18 materials with colors from your implementation
    static constexpr std::array<MaterialInfo, 18> s_materials = {{
        {0,  "Air",       0.0f,  0.0f,  0.0f,  "Empty space"},
        {1,  "Sand",      0.9f,  0.85f, 0.6f,  "Falls like powder"},
        {2,  "Water",     0.2f,  0.5f,  1.0f,  "Flows, freezes, evaporates"},
        {3,  "Stone",     0.5f,  0.5f,  0.5f,  "Solid, static block"},
        {4,  "Dirt",      0.6f,  0.4f,  0.2f,  "Solid, static block"},
        {5,  "Wood",      0.4f,  0.25f, 0.1f,  "Flammable solid"},
        {6,  "Fire",      1.0f,  0.4f,  0.0f,  "Spreads, creates smoke"},
        {7,  "Lava",      1.0f,  0.3f,  0.0f,  "Hot flowing liquid, ignites"},
        {8,  "Ice",       0.7f,  0.9f,  1.0f,  "Frozen water, static"},
        {9,  "Oil",       0.1f,  0.08f, 0.05f, "Flammable liquid"},
        {10, "Glass",     0.8f,  0.9f,  1.0f,  "Transparent solid"},
        {11, "Smoke",     0.3f,  0.3f,  0.35f, "Rises and dissipates"},
        {12, "Acid",      0.2f,  0.9f,  0.2f,  "Dissolves solids, neutralizes in water"},
        {13, "Honey",     0.95f, 0.75f, 0.2f,  "Super viscous liquid"},
        {14, "Concrete",  0.6f,  0.6f,  0.65f, "Liquid that hardens to stone"},
        {15, "Gunpowder", 0.2f,  0.2f,  0.25f, "Explosive powder, chain reactions"},
        {16, "Crystal",   0.7f,  0.3f,  0.9f,  "Decorative static block"},
        {17, "Steam",     0.9f,  0.95f, 1.0f,  "Rises quickly, dissipates fast"}
    }};

    bool m_windowOpen = true;
};

} // namespace VENPOD::UI
