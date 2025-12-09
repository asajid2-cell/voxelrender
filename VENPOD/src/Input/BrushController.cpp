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
    float scrollDelta,
    const uint32_t* voxelGridData,
    size_t voxelGridSize)
{
    // Update painting/erasing state
    m_isPainting = leftButtonDown;
    m_isErasing = rightButtonDown;

    // Adjust radius with scroll wheel
    if (scrollDelta != 0.0f) {
        SetRadius(m_radius + scrollDelta);
    }

    // Calculate ray direction from NDC (same as GPU raymarcher)
    float tanHalfFov = std::tan(fov * 0.5f);
    glm::vec3 rayDir = glm::normalize(
        cameraForward +
        cameraRight * mouseNDC.x * tanHalfFov * aspectRatio +
        cameraUp * mouseNDC.y * tanHalfFov
    );

    // Use DDA raycasting if voxel data is available
    if (voxelGridData != nullptr && voxelGridSize > 0) {
        glm::ivec3 hitVoxel;
        glm::ivec3 hitNormal;

        // Perform DDA raycasting to find first solid voxel
        if (DDAVoxelRaycast(cameraPos, rayDir, voxelGridData, voxelGridSize, hitVoxel, hitNormal)) {
            glm::vec3 targetVoxel;

            // ERASE: Place brush ON the voxel we hit (to erase it)
            // PAINT: Place brush ADJACENT to the voxel (to add new voxels)
            if (rightButtonDown) {
                // Erasing - target the hit voxel itself
                targetVoxel = glm::vec3(hitVoxel);
            } else {
                // Painting - target the adjacent empty voxel
                targetVoxel = glm::vec3(hitVoxel) + glm::vec3(hitNormal);
            }

            // Set brush position to center of the target voxel
            m_brushPosition = targetVoxel + glm::vec3(0.5f);

            // Clamp to grid bounds
            m_brushPosition = glm::clamp(m_brushPosition,
                m_gridMin + glm::vec3(0.5f),
                m_gridMax - glm::vec3(0.5f));

            m_hasValidPosition = true;
        } else {
            // No solid voxel hit - don't show preview
            m_hasValidPosition = false;
        }
    } else {
        // Fallback: No voxel data available, use ray-AABB intersection
        // This provides a degraded experience but prevents crashes
        float tMin, tMax;
        if (RayBoxIntersect(cameraPos, rayDir, m_gridMin, m_gridMax, tMin, tMax)) {
            // Place at fixed distance along ray (10 voxels ahead)
            constexpr float defaultDistance = 10.0f;
            float t = (tMin > 0.0f) ? tMin + defaultDistance : defaultDistance;

            glm::vec3 hitPos = cameraPos + rayDir * t;

            // Clamp to grid bounds
            hitPos = glm::clamp(hitPos,
                m_gridMin + glm::vec3(0.5f),
                m_gridMax - glm::vec3(0.5f));

            m_brushPosition = hitPos;
            m_hasValidPosition = true;
        } else {
            m_hasValidPosition = false;
        }
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

bool BrushController::DDAVoxelRaycast(
    const glm::vec3& rayOrigin,
    const glm::vec3& rayDir,
    const uint32_t* voxelGridData,
    size_t voxelGridSize,
    glm::ivec3& hitVoxel,
    glm::ivec3& hitNormal) const
{
    // Maximum raycast distance and steps (same as GPU shader)
    constexpr float maxDist = 1024.0f;
    constexpr int maxSteps = 512;

    // Find ray entry point into grid
    float tMin, tMax;
    if (!RayBoxIntersect(rayOrigin, rayDir, m_gridMin, m_gridMax, tMin, tMax)) {
        return false;  // Ray doesn't intersect grid
    }

    // Start raymarching from grid entry point (or ray origin if inside grid)
    // Add small epsilon to ensure we're inside the grid when entering from outside
    float entryT = std::max(tMin, 0.0f);
    if (tMin > 0.0f) entryT += 0.001f;  // Move slightly inside grid boundary
    glm::vec3 startPos = rayOrigin + rayDir * entryT;

    // Start position in voxel grid (integer coordinates)
    // Add small epsilon to floor to avoid precision issues at boundaries
    glm::ivec3 voxelPos = glm::ivec3(glm::floor(startPos + glm::vec3(1e-4f)));

    // DDA setup - matches GPU implementation in PS_Raymarch.hlsl
    glm::vec3 deltaDist = glm::abs(1.0f / rayDir);
    glm::ivec3 step = glm::ivec3(glm::sign(rayDir));

    // Calculate initial side distances
    glm::vec3 sideDist;
    sideDist.x = (rayDir.x > 0.0f) ? (voxelPos.x + 1.0f - startPos.x) : (startPos.x - voxelPos.x);
    sideDist.y = (rayDir.y > 0.0f) ? (voxelPos.y + 1.0f - startPos.y) : (startPos.y - voxelPos.y);
    sideDist.z = (rayDir.z > 0.0f) ? (voxelPos.z + 1.0f - startPos.z) : (startPos.z - voxelPos.z);
    sideDist *= deltaDist;

    glm::ivec3 normal(0, 1, 0);  // Default normal
    float dist = 0.0f;

    // DDA traversal - step through voxel grid
    for (int i = 0; i < maxSteps; ++i) {
        // Check if current voxel is solid
        uint32_t voxel = GetVoxelAt(voxelPos, voxelGridData, voxelGridSize);
        uint32_t material = GetMaterialFromVoxel(voxel);

        // Hit non-air voxel?
        if (material != 0) {  // 0 = MAT_AIR
            hitVoxel = voxelPos;
            hitNormal = normal;
            return true;
        }

        // Step to next voxel boundary (same logic as GPU shader)
        if (sideDist.x < sideDist.y) {
            if (sideDist.x < sideDist.z) {
                sideDist.x += deltaDist.x;
                voxelPos.x += step.x;
                normal = glm::ivec3(-step.x, 0, 0);
                dist = sideDist.x;
            } else {
                sideDist.z += deltaDist.z;
                voxelPos.z += step.z;
                normal = glm::ivec3(0, 0, -step.z);
                dist = sideDist.z;
            }
        } else {
            if (sideDist.y < sideDist.z) {
                sideDist.y += deltaDist.y;
                voxelPos.y += step.y;
                normal = glm::ivec3(0, -step.y, 0);
                dist = sideDist.y;
            } else {
                sideDist.z += deltaDist.z;
                voxelPos.z += step.z;
                normal = glm::ivec3(0, 0, -step.z);
                dist = sideDist.z;
            }
        }

        // Stop if we've gone too far
        if (dist > maxDist) {
            break;
        }
    }

    return false;  // No solid voxel hit
}

uint32_t BrushController::GetVoxelAt(
    const glm::ivec3& pos,
    const uint32_t* voxelGridData,
    size_t voxelGridSize) const
{
    // Bounds check
    if (pos.x < 0 || pos.x >= static_cast<int>(m_gridMax.x) ||
        pos.y < 0 || pos.y >= static_cast<int>(m_gridMax.y) ||
        pos.z < 0 || pos.z >= static_cast<int>(m_gridMax.z)) {
        return 0;  // Out of bounds = air
    }

    // Calculate linear index (same as GPU: x + y*sizeX + z*sizeX*sizeY)
    uint32_t gridSizeX = static_cast<uint32_t>(m_gridMax.x);
    uint32_t gridSizeY = static_cast<uint32_t>(m_gridMax.y);
    size_t idx = static_cast<size_t>(pos.x) +
                 static_cast<size_t>(pos.y) * gridSizeX +
                 static_cast<size_t>(pos.z) * gridSizeX * gridSizeY;

    // Bounds check on array
    if (idx >= voxelGridSize) {
        return 0;  // Out of bounds = air
    }

    return voxelGridData[idx];
}

} // namespace VENPOD::Input
