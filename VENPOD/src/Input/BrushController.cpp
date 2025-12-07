#include "BrushController.h"
#include <cmath>
#include <algorithm>

namespace VENPOD::Input {

void BrushController::Initialize() {
    m_brushPosition = glm::vec3(64.0f);  // Center of default grid
    m_hasValidPosition = false;
    m_isPainting = false;
    m_isErasing = false;
    m_material = 1;  // Sand
    m_radius = 5.0f;
    m_strength = 1.0f;
    m_mode = BrushMode::Paint;
    m_shape = BrushShape::Sphere;
}

void BrushController::UpdateFromMouse(
    const glm::vec2& mouseNDC,
    const glm::vec3& cameraPos,
    const glm::vec3& cameraForward,
    const glm::vec3& cameraRight,
    const glm::vec3& cameraUp,
    float fov,
    float aspectRatio,
    bool leftButtonDown,
    bool rightButtonDown,
    float scrollDelta)
{
    // Update painting/erasing state
    m_isPainting = leftButtonDown;
    m_isErasing = rightButtonDown;

    // Adjust radius with scroll wheel
    if (scrollDelta != 0.0f) {
        SetRadius(m_radius + scrollDelta);
    }

    // Calculate ray direction from NDC
    float tanHalfFov = std::tan(fov * 0.5f);
    glm::vec3 rayDir = glm::normalize(
        cameraForward +
        cameraRight * mouseNDC.x * tanHalfFov * aspectRatio +
        cameraUp * mouseNDC.y * tanHalfFov
    );

    // Ray-box intersection with voxel grid
    float tMin, tMax;
    if (RayBoxIntersect(cameraPos, rayDir, m_gridMin, m_gridMax, tMin, tMax)) {
        // Use DDA-style raycasting to find placement position
        // We'll raycast a fixed distance and place voxel slightly offset along the ray
        constexpr float maxDistance = 50.0f;  // Maximum reach distance

        float t = (tMin > 0.0f) ? tMin : 0.0f;  // Start from grid entry or camera if inside
        t = std::min(t + 10.0f, maxDistance);   // Default 10 voxels ahead, max 50

        // Calculate hit position
        glm::vec3 hitPos = cameraPos + rayDir * t;

        // Clamp to grid bounds
        hitPos = glm::clamp(hitPos,
            m_gridMin + glm::vec3(0.5f),
            m_gridMax - glm::vec3(0.5f));

        // Get voxel coordinate the ray is pointing at
        glm::ivec3 voxelCoord = glm::ivec3(glm::floor(hitPos));

        // Determine which face was hit by checking which axis is closest to a boundary
        glm::vec3 fractional = hitPos - glm::vec3(voxelCoord);
        glm::vec3 toFaceMin = fractional;  // Distance to min faces (0.0)
        glm::vec3 toFaceMax = glm::vec3(1.0f) - fractional;  // Distance to max faces (1.0)

        // Find the closest face and its normal
        glm::vec3 faceNormal(0.0f, 1.0f, 0.0f);  // Default to placing above
        float minDist = toFaceMax.y;  // Start with top face

        // Check all 6 faces
        if (toFaceMin.x < minDist && rayDir.x > 0.0f) { minDist = toFaceMin.x; faceNormal = glm::vec3(-1.0f, 0.0f, 0.0f); }
        if (toFaceMax.x < minDist && rayDir.x < 0.0f) { minDist = toFaceMax.x; faceNormal = glm::vec3( 1.0f, 0.0f, 0.0f); }
        if (toFaceMin.y < minDist && rayDir.y > 0.0f) { minDist = toFaceMin.y; faceNormal = glm::vec3( 0.0f,-1.0f, 0.0f); }
        if (toFaceMax.y < minDist && rayDir.y < 0.0f) { minDist = toFaceMax.y; faceNormal = glm::vec3( 0.0f, 1.0f, 0.0f); }
        if (toFaceMin.z < minDist && rayDir.z > 0.0f) { minDist = toFaceMin.z; faceNormal = glm::vec3( 0.0f, 0.0f,-1.0f); }
        if (toFaceMax.z < minDist && rayDir.z < 0.0f) { minDist = toFaceMax.z; faceNormal = glm::vec3( 0.0f, 0.0f, 1.0f); }

        // Place brush at adjacent voxel position (outside the hit voxel)
        // Add epsilon offset along normal to ensure we're in the empty space
        constexpr float epsilon = 0.001f;
        m_brushPosition = glm::vec3(voxelCoord) + glm::vec3(0.5f) + faceNormal * (0.5f + epsilon);

        // Final clamp to ensure we're in bounds
        m_brushPosition = glm::clamp(m_brushPosition,
            m_gridMin + glm::vec3(0.5f),
            m_gridMax - glm::vec3(0.5f));

        m_hasValidPosition = true;
    } else {
        m_hasValidPosition = false;
    }
}

BrushConstants BrushController::GetBrushConstants(
    uint32_t gridSizeX,
    uint32_t gridSizeY,
    uint32_t gridSizeZ,
    uint32_t seed) const
{
    // Update grid bounds
    const_cast<BrushController*>(this)->m_gridMax = glm::vec3(
        static_cast<float>(gridSizeX),
        static_cast<float>(gridSizeY),
        static_cast<float>(gridSizeZ)
    );

    BrushConstants constants{};
    constants.positionX = m_brushPosition.x;
    constants.positionY = m_brushPosition.y;
    constants.positionZ = m_brushPosition.z;
    constants.radius = m_radius;
    constants.material = m_isErasing ? 0 : m_material;  // 0 = air for erasing
    constants.mode = static_cast<uint32_t>(m_isErasing ? BrushMode::Erase : m_mode);
    constants.shape = static_cast<uint32_t>(m_shape);
    constants.strength = m_strength;
    constants.gridSizeX = gridSizeX;
    constants.gridSizeY = gridSizeY;
    constants.gridSizeZ = gridSizeZ;
    constants.seed = seed;

    return constants;
}

void BrushController::NextMaterial() {
    m_material++;
    if (m_material > m_maxMaterial) {
        m_material = m_minMaterial;
    }
}

void BrushController::PrevMaterial() {
    if (m_material <= m_minMaterial) {
        m_material = m_maxMaterial;
    } else {
        m_material--;
    }
}

bool BrushController::RayBoxIntersect(
    const glm::vec3& rayOrigin,
    const glm::vec3& rayDir,
    const glm::vec3& boxMin,
    const glm::vec3& boxMax,
    float& tMin,
    float& tMax) const
{
    // Safe inverse direction - avoid division by zero
    const float epsilon = 1e-6f;
    glm::vec3 invDir = glm::vec3(
        std::abs(rayDir.x) > epsilon ? 1.0f / rayDir.x : 1e6f,
        std::abs(rayDir.y) > epsilon ? 1.0f / rayDir.y : 1e6f,
        std::abs(rayDir.z) > epsilon ? 1.0f / rayDir.z : 1e6f
    );

    glm::vec3 t0 = (boxMin - rayOrigin) * invDir;
    glm::vec3 t1 = (boxMax - rayOrigin) * invDir;

    glm::vec3 tNear = glm::min(t0, t1);
    glm::vec3 tFar = glm::max(t0, t1);

    tMin = std::max(std::max(tNear.x, tNear.y), tNear.z);
    tMax = std::min(std::min(tFar.x, tFar.y), tFar.z);

    return tMax >= tMin && tMax >= 0.0f;
}

} // namespace VENPOD::Input
