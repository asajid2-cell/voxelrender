// =============================================================================
// VENPOD - Editable SVDAG raymarch (P2)
// A SEPARATE, deliberately simple fullscreen pass that raymarches the pointer-
// indirection Sparse Voxel DAG (de-duplicated from FarVoxelOctree) and renders
// its leaves as TRUE voxel boxes. This is NOT bolted into PS_Raymarch (which is
// at the NVIDIA JIT/TDR cliff). It owns the mid/far background where it hits and
// is transparent (alpha 0) on miss, so the existing sky/fallback shows through.
// Compositing behind the near mesh is done by the pipeline via STENCIL ownership
// (stencil == 0), not depth — see Renderer m_dagRaymarchPipeline.
//
// DAG node format (matches FarVoxelOctree::DagNodeGpu / BuildDagFromTree):
//   DagNode{ childPtrBase, childMask, material, flags }  (16B, logical-id addressable)
//   child node index = DagChildPointers[childPtrBase + countbits(childMask & ((1<<c)-1))]
//   leaf: flags & 1, or childMask == 0, or childPtrBase == 0xFFFFFFFF.
// =============================================================================

#include "../Common/SharedTypes.hlsli"

cbuffer FrameConstantsCB : register(b0) {
    FrameConstants frame;
}

struct DagNode {
    uint childPtrBase;
    uint childMask;
    uint material;
    uint flags;
};

struct DagPage {
    int  originX;
    int  originY;
    int  originZ;
    uint rootNode;
};

StructuredBuffer<DagNode> DagNodes        : register(t0);
StructuredBuffer<uint>    DagChildPointers : register(t1);
StructuredBuffer<DagPage> DagPages        : register(t2);
StructuredBuffer<uint>    DagPageIndex     : register(t3);  // reuse far tree's denseIndex->page map

struct PSInput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

struct PSOutput {
    float4 color : SV_Target;
};

static const uint  DAG_LEAF_FLAG = 1u;
static const uint  DAG_INTERIOR_LEAF_FLAG = 2u;
static const uint  DAG_INVALID   = 0xFFFFFFFFu;
static const float DAG_BIG       = 1e30f;

// --- box intersection (mirrors PS_Raymarch IntersectBox / IntersectBoxWithNormal) ---
bool DagIntersectBox(float3 ro, float3 rd, float3 bmin, float3 bmax, out float tmin, out float tmax) {
    float3 safeDir = float3(
        abs(rd.x) < 1e-6f ? (rd.x < 0.0f ? -1e-6f : 1e-6f) : rd.x,
        abs(rd.y) < 1e-6f ? (rd.y < 0.0f ? -1e-6f : 1e-6f) : rd.y,
        abs(rd.z) < 1e-6f ? (rd.z < 0.0f ? -1e-6f : 1e-6f) : rd.z);
    float3 invDir = 1.0f / safeDir;
    float3 t0 = (bmin - ro) * invDir;
    float3 t1 = (bmax - ro) * invDir;
    float3 tsmall = min(t0, t1);
    float3 tbig = max(t0, t1);
    tmin = max(max(tsmall.x, tsmall.y), tsmall.z);
    tmax = min(min(tbig.x, tbig.y), tbig.z);
    return tmax >= max(tmin, 0.0f);
}

bool DagIntersectBoxN(float3 ro, float3 rd, float3 bmin, float3 bmax,
                      out float tmin, out float tmax, out float3 normal) {
    float3 safeDir = float3(
        abs(rd.x) < 1e-6f ? (rd.x < 0.0f ? -1e-6f : 1e-6f) : rd.x,
        abs(rd.y) < 1e-6f ? (rd.y < 0.0f ? -1e-6f : 1e-6f) : rd.y,
        abs(rd.z) < 1e-6f ? (rd.z < 0.0f ? -1e-6f : 1e-6f) : rd.z);
    float3 invDir = 1.0f / safeDir;
    float3 t0 = (bmin - ro) * invDir;
    float3 t1 = (bmax - ro) * invDir;
    float3 tsmall = min(t0, t1);
    float3 tbig = max(t0, t1);
    tmin = max(max(tsmall.x, tsmall.y), tsmall.z);
    tmax = min(min(tbig.x, tbig.y), tbig.z);
    normal = float3(0.0f, 1.0f, 0.0f);
    if (tsmall.x >= tsmall.y && tsmall.x >= tsmall.z) {
        normal = float3(rd.x < 0.0f ? 1.0f : -1.0f, 0.0f, 0.0f);
    } else if (tsmall.y >= tsmall.z) {
        normal = float3(0.0f, rd.y < 0.0f ? 1.0f : -1.0f, 0.0f);
    } else {
        normal = float3(0.0f, 0.0f, rd.z < 0.0f ? 1.0f : -1.0f);
    }
    return tmax >= max(tmin, 0.0f);
}

uint DagChildNodeIndex(DagNode node, uint childOrdinal) {
    uint preceding = node.childMask & ((1u << childOrdinal) - 1u);
    return DagChildPointers[node.childPtrBase + countbits(preceding)];
}

float3 DagMaterialColor(uint material) {
    // Matches FarVoxelOctree material ids: 1 sand, 2 water, 3 stone, 4 dirt.
    if (material == 2u) return float3(0.16f, 0.34f, 0.58f);  // water
    if (material == 1u) return float3(0.78f, 0.71f, 0.50f);  // sand
    if (material == 3u) return float3(0.46f, 0.46f, 0.49f);  // stone
    if (material == 4u) return float3(0.42f, 0.31f, 0.21f);  // dirt
    return float3(0.40f, 0.45f, 0.30f);                       // default (grassy)
}

// Hierarchical traversal of one DAG page; returns the nearest non-air leaf box hit.
bool TraverseDagPage(float3 ro, float3 rd, uint rootNode, float3 pageMin, float pageSize,
                     uint nodeCount, inout float nearestT, inout float3 nearestN, inout uint nearestMat) {
    uint  nodeStack[64];
    float3 minStack[64];
    float sizeStack[64];
    int stackCount = 0;
    nodeStack[0] = rootNode;
    minStack[0] = pageMin;
    sizeStack[0] = pageSize;
    stackCount = 1;

    bool hit = false;
    [loop]
    while (stackCount > 0) {
        stackCount--;
        uint nodeIndex = nodeStack[stackCount];
        float3 nodeMin = minStack[stackCount];
        float nodeSize = sizeStack[stackCount];
        if (nodeIndex >= nodeCount) {
            continue;
        }

        float tNear, tFar;
        float3 boxNormal;
        if (!DagIntersectBoxN(ro, rd, nodeMin, nodeMin + nodeSize, tNear, tFar, boxNormal)) {
            continue;
        }
        if (tFar < 0.0f || tNear > nearestT) {
            continue;
        }

        DagNode node = DagNodes[nodeIndex];
        bool leaf = (node.flags & DAG_LEAF_FLAG) != 0u ||
                    node.childMask == 0u ||
                    node.childPtrBase == DAG_INVALID;
        if (leaf) {
            // Skip air leaves AND interior-leaf cells: the latter are large conservative
            // solid-interior volumes (collapsed below the surface), not drawable surface
            // geometry. Rendering them as boxes creates false surfaces (the original
            // TraverseFarVoxelPage treats them via heightfield recovery, not AABBs).
            if (node.material == 0u || (node.flags & DAG_INTERIOR_LEAF_FLAG) != 0u) {
                continue;
            }
            float candidateT = max(tNear, 0.0f);
            if (candidateT < nearestT) {
                nearestT = candidateT;
                nearestN = boxNormal;
                nearestMat = node.material;
                hit = true;
            }
            continue;
        }

        // Gather existing children, insertion-sorted near->far, then push far-first so
        // the nearest is popped first (aggressive nearestT culling -> shallow stack).
        float childSize = nodeSize * 0.5f;
        uint childNodes[8];
        float3 childMins[8];
        float childNear[8];
        int childCount = 0;
        [unroll]
        for (uint child = 0; child < 8; ++child) {
            if ((node.childMask & (1u << child)) == 0u) {
                continue;
            }
            float3 childMin = nodeMin + float3(
                (child & 1u) ? childSize : 0.0f,
                (child & 2u) ? childSize : 0.0f,
                (child & 4u) ? childSize : 0.0f);
            float ctn, ctf;
            if (!DagIntersectBox(ro, rd, childMin, childMin + childSize, ctn, ctf) ||
                ctf < 0.0f || ctn > nearestT) {
                continue;
            }
            int insertAt = childCount;
            [loop]
            while (insertAt > 0 && ctn < childNear[insertAt - 1]) {
                childNodes[insertAt] = childNodes[insertAt - 1];
                childMins[insertAt] = childMins[insertAt - 1];
                childNear[insertAt] = childNear[insertAt - 1];
                insertAt--;
            }
            childNodes[insertAt] = DagChildNodeIndex(node, child);
            childMins[insertAt] = childMin;
            childNear[insertAt] = ctn;
            childCount++;
        }
        [loop]
        for (int ci = childCount - 1; ci >= 0 && stackCount < 64; --ci) {
            nodeStack[stackCount] = childNodes[ci];
            minStack[stackCount] = childMins[ci];
            sizeStack[stackCount] = childSize;
            stackCount++;
        }
    }
    return hit;
}

// Distance to the next page column boundary (2D DDA over the x/z page grid).
float DagDistanceToPageExit(float3 pos, float3 rd, float pageSize) {
    float cellX = floor(pos.x / pageSize) * pageSize;
    float cellZ = floor(pos.z / pageSize) * pageSize;
    float tX = DAG_BIG;
    float tZ = DAG_BIG;
    if (rd.x > 1e-6f)      tX = (cellX + pageSize - pos.x) / rd.x;
    else if (rd.x < -1e-6f) tX = (cellX - pos.x) / rd.x;
    if (rd.z > 1e-6f)      tZ = (cellZ + pageSize - pos.z) / rd.z;
    else if (rd.z < -1e-6f) tZ = (cellZ - pos.z) / rd.z;
    return max(min(tX, tZ), 0.0f) + pageSize * 0.001f;
}

PSOutput main(PSInput input) {
    PSOutput output;
    output.color = float4(0.0f, 0.0f, 0.0f, 0.0f);

    float3 ro = frame.cameraPosition.xyz;
    float3 forward = frame.cameraForward.xyz;
    float3 right = frame.cameraRight.xyz;
    float3 up = frame.cameraUp.xyz;
    float fov = frame.cameraPosition.w;
    float aspect = frame.cameraForward.w;

    float2 ndc = input.uv * 2.0f - 1.0f;
    ndc.y = -ndc.y;
    float tanHalfFov = tan(fov * 0.5f);
    float3 rd = normalize(forward + right * ndc.x * tanHalfFov * aspect + up * ndc.y * tanHalfFov);

    uint  enabled    = (uint)frame.farFieldParams.x;
    uint  pageCount  = (uint)frame.farFieldParams.y;
    uint  nodeCount  = (uint)frame.farFieldParams.z;
    float pageSize   = frame.farFieldParams.w;
    int   pageRadius = (int)frame.farFieldGridParams.x;
    int   pageSide   = (int)frame.farFieldGridParams.y;
    if (enabled == 0u || pageSize <= 0.0f || pageSide <= 0 || nodeCount == 0u) {
        return output;  // DAG not ready -> fully transparent, background owns the pixel
    }

    float nearestT = DAG_BIG;
    float3 nearestN = float3(0.0f, 1.0f, 0.0f);
    uint nearestMat = 0u;
    bool anyHit = false;

    float farMax = (float)pageRadius * pageSize * 2.0f;
    float t = 0.0f;
    [loop]
    for (int step = 0; step < 96 && t < farMax && t < nearestT; ++step) {
        float3 pos = ro + rd * t;
        int px = (int)floor(pos.x / pageSize);
        int pz = (int)floor(pos.z / pageSize);
        if (px >= -pageRadius && px <= pageRadius && pz >= -pageRadius && pz <= pageRadius) {
            uint denseIndex = (uint)((pz + pageRadius) * pageSide + (px + pageRadius));
            uint pageIndex = DagPageIndex[denseIndex];
            if (pageIndex != DAG_INVALID && pageIndex < pageCount) {
                DagPage page = DagPages[pageIndex];
                float3 pageMin = float3((float)page.originX, (float)page.originY, (float)page.originZ);
                float tn, tf;
                if (DagIntersectBox(ro, rd, pageMin, pageMin + pageSize, tn, tf) &&
                    tf >= 0.0f && tn <= nearestT) {
                    if (TraverseDagPage(ro, rd, page.rootNode, pageMin, pageSize, nodeCount,
                                        nearestT, nearestN, nearestMat)) {
                        anyHit = true;
                    }
                }
            }
        }
        t += DagDistanceToPageExit(pos, rd, pageSize);
    }

    if (!anyHit) {
        return output;  // miss -> alpha 0, existing sky/fallback shows through
    }

    float3 baseColor = DagMaterialColor(nearestMat);
    float3 sunDir = normalize(frame.sunDirection.xyz + float3(0.0f, 1e-4f, 0.0f));
    float diffuse = saturate(dot(nearestN, sunDir)) * 0.7f + 0.3f;
    output.color = float4(baseColor * diffuse, 1.0f);  // alpha 1 -> DAG owns this pixel
    return output;
}
