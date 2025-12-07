#pragma once

// =============================================================================
// VENPOD Brush Controller - Manages brush state and material painting
// =============================================================================

#include <glm/glm.hpp>
#include <cstdint>

namespace VENPOD::Input {

// Brush modes for different painting behaviors
enum class BrushMode : uint8_t {
    Paint = 0,      // Add material
    Erase = 1,      // Remove material (set to air)
    Replace = 2,    // Replace existing non-air voxels
    Fill = 3        // Fill enclosed area
};

// Brush shape for SDF painting
enum class BrushShape : uint8_t {
    Sphere = 0,
    Cube = 1,
    Cylinder = 2
};

// Brush constants passed to compute shader
struct BrushConstants {
    float positionX;        // Brush center X in voxel space
    float positionY;        // Brush center Y in voxel space
    float positionZ;        // Brush center Z in voxel space
    float radius;           // Brush radius in voxels

    uint32_t material;      // Material ID to paint
    uint32_t mode;          // BrushMode
    uint32_t shape;         // BrushShape
    float strength;         // 0-1, affects variant/noise

    uint32_t gridSizeX;
    uint32_t gridSizeY;
    uint32_t gridSizeZ;
    uint32_t seed;          // Random seed for variant generation
};

class BrushController {
public:
    BrushController() = default;
    ~BrushController() = default;

    // Non-copyable
    BrushController(const BrushController&) = delete;
    BrushController& operator=(const BrushController&) = delete;

    // Initialize with default settings
    void Initialize();

    // Update brush position from screen coordinates
    // Returns true if brush should paint this frame
    void UpdateFromMouse(
        const glm::vec2& mouseNDC,
        const glm::vec3& cameraPos,
        const glm::vec3& cameraForward,
        const glm::vec3& cameraRight,
        const glm::vec3& cameraUp,
        float fov,
        float aspectRatio,
        bool leftButtonDown,
        bool rightButtonDown,
        float scrollDelta
    );

    // Get brush constants for shader
    BrushConstants GetBrushConstants(
        uint32_t gridSizeX,
        uint32_t gridSizeY,
        uint32_t gridSizeZ,
        uint32_t seed
    ) const;

    // Material selection
    void SetMaterial(uint32_t material) { m_material = material; }
    uint32_t GetMaterial() const { return m_material; }
    void NextMaterial();
    void PrevMaterial();

    // Brush size
    void SetRadius(float radius) { m_radius = glm::clamp(radius, m_minRadius, m_maxRadius); }
    float GetRadius() const { return m_radius; }
    void IncreaseRadius() { SetRadius(m_radius + 1.0f); }
    void DecreaseRadius() { SetRadius(m_radius - 1.0f); }

    // Brush mode
    void SetMode(BrushMode mode) { m_mode = mode; }
    BrushMode GetMode() const { return m_mode; }

    // Brush shape
    void SetShape(BrushShape shape) { m_shape = shape; }
    BrushShape GetShape() const { return m_shape; }

    // State queries
    bool IsPainting() const { return m_isPainting; }
    bool IsErasing() const { return m_isErasing; }
    glm::vec3 GetBrushPosition() const { return m_brushPosition; }
    bool HasValidPosition() const { return m_hasValidPosition; }

    // Set grid bounds for ray intersection
    void SetGridBounds(float sizeX, float sizeY, float sizeZ) {
        m_gridMax = glm::vec3(sizeX, sizeY, sizeZ);
    }

private:
    // Ray-AABB intersection for finding brush position
    bool RayBoxIntersect(
        const glm::vec3& rayOrigin,
        const glm::vec3& rayDir,
        const glm::vec3& boxMin,
        const glm::vec3& boxMax,
        float& tMin,
        float& tMax
    ) const;

    // Current brush state
    glm::vec3 m_brushPosition{0.0f};
    bool m_hasValidPosition = false;
    bool m_isPainting = false;
    bool m_isErasing = false;

    // Brush settings
    uint32_t m_material = 1;  // Default: sand
    float m_radius = 5.0f;
    float m_strength = 1.0f;
    BrushMode m_mode = BrushMode::Paint;
    BrushShape m_shape = BrushShape::Sphere;

    // Limits
    static constexpr float m_minRadius = 1.0f;
    static constexpr float m_maxRadius = 32.0f;
    static constexpr uint32_t m_minMaterial = 1;  // 0 is air
    static constexpr uint32_t m_maxMaterial = 10; // Up to glass

    // Grid bounds (updated each frame)
    glm::vec3 m_gridMin{0.0f};
    glm::vec3 m_gridMax{128.0f};
};

} // namespace VENPOD::Input
