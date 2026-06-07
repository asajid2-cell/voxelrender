#include "../Common/SharedTypes.hlsli"

struct SparsePhysicsPacketConstants {
    uint packetCount;
    uint frameIndex;
    uint pageTableCapacity;
    uint editDeltaCount;
    uint editDeltaRangeCount;
    uint editDeltaRangeTableCapacity;
    uint padding1;
    uint padding2;
};

ConstantBuffer<SparsePhysicsPacketConstants> gConstants : register(b0);
StructuredBuffer<SparsePhysicsWorkPacket> gPackets : register(t0);
StructuredBuffer<BrickPageEntry> gPageTable : register(t1);
StructuredBuffer<uint> gBrickPool : register(t2);
StructuredBuffer<SparseEditDelta> gEditDeltas : register(t3);
StructuredBuffer<SparseEditDeltaRange> gEditDeltaRanges : register(t4);
StructuredBuffer<uint> gEditDeltaRangeTable : register(t5);
RWStructuredBuffer<SparsePhysicsPacketResult> gResults : register(u0);
RWStructuredBuffer<uint> gDiagnostics : register(u1);

static const uint SPARSE_INVALID_PAGE = 0xFFFFFFFFu;
static const uint SPARSE_TOMBSTONE_PAGE = 0xFFFFFFFEu;
static const uint SPARSE_PAGE_TABLE_LOOKUP_PROBES = 256u;
static const uint SPARSE_BRICK_SIZE = 16u;
static const uint SPARSE_BRICK_VOXEL_COUNT = 4096u;
static const uint PACKET_STATUS_CONSUMED = 1u;
static const uint PACKET_STATUS_HAS_EXPECTED_PAGE = 2u;
static const uint PACKET_STATUS_PAGE_MATCH = 4u;
static const uint PACKET_STATUS_PAGE_STALE = 8u;
static const uint PACKET_STATUS_PROPOSAL = 16u;
static const uint PACKET_STATUS_MISSING_BELOW = 32u;
static const uint PACKET_STATUS_EDIT_DELTA_HIT = 64u;

uint UnpackMaterial(uint voxel) {
    return voxel & 0xFFu;
}

uint PackLocal(uint3 localVoxel) {
    return (localVoxel.x & 0xFFu) |
        ((localVoxel.y & 0xFFu) << 8) |
        ((localVoxel.z & 0xFFu) << 16);
}

uint3 UnpackLocal(uint packed) {
    return uint3(
        packed & 0xFFu,
        (packed >> 8) & 0xFFu,
        (packed >> 16) & 0xFFu);
}

uint SparseLocalIndex(uint3 localVoxel) {
    return localVoxel.x +
        localVoxel.y * SPARSE_BRICK_SIZE +
        localVoxel.z * SPARSE_BRICK_SIZE * SPARSE_BRICK_SIZE;
}

uint HashSparseBrickCoord(int3 coord) {
    uint hash = 2166136261u;
    hash = (hash ^ (uint)coord.x) * 16777619u;
    hash = (hash ^ (uint)coord.y) * 16777619u;
    hash = (hash ^ (uint)coord.z) * 16777619u;
    return hash;
}

bool LookupSparseBrick(int3 brickCoord, out BrickPageEntry result) {
    result = (BrickPageEntry)0;
    const uint capacity = gConstants.pageTableCapacity;
    if (capacity == 0u || (capacity & (capacity - 1u)) != 0u) {
        return false;
    }

    const uint mask = capacity - 1u;
    const uint start = HashSparseBrickCoord(brickCoord) & mask;
    [loop]
    for (uint probe = 0u; probe < SPARSE_PAGE_TABLE_LOOKUP_PROBES; ++probe) {
        const uint slot = (start + probe) & mask;
        const BrickPageEntry entry = gPageTable[slot];
        if (entry.pageIndex == SPARSE_INVALID_PAGE) {
            return false;
        }
        if (entry.pageIndex == SPARSE_TOMBSTONE_PAGE) {
            continue;
        }
        if (all(entry.coord == brickCoord)) {
            result = entry;
            return entry.generation != 0u;
        }
    }
    return false;
}

bool TrySampleResidentVoxel(int3 brickCoord, uint3 localVoxel, out uint voxel) {
    voxel = 0u;
    BrickPageEntry entry;
    if (!LookupSparseBrick(brickCoord, entry)) {
        return false;
    }
    if (entry.pageIndex == SPARSE_INVALID_PAGE ||
        entry.pageIndex == SPARSE_TOMBSTONE_PAGE) {
        return false;
    }
    const uint index = entry.pageIndex * SPARSE_BRICK_VOXEL_COUNT + SparseLocalIndex(localVoxel);
    voxel = gBrickPool[index];
    return true;
}

bool TrySampleEditDelta(int3 brickCoord, uint3 localVoxel, out uint voxel, out uint revision) {
    voxel = 0u;
    revision = 0u;
    const uint packedLocal = PackLocal(localVoxel);
    const uint deltaCount = min(gConstants.editDeltaCount, 8192u);
    const uint rangeCount = min(gConstants.editDeltaRangeCount, 2048u);
    const uint rangeTableCapacity = gConstants.editDeltaRangeTableCapacity;
    uint bestRevision = 0u;
    bool found = false;

    uint matchedRangeIndex = 0xFFFFFFFFu;
    if (rangeTableCapacity != 0u && (rangeTableCapacity & (rangeTableCapacity - 1u)) == 0u) {
        const uint mask = rangeTableCapacity - 1u;
        const uint start = HashSparseBrickCoord(brickCoord) & mask;
        [loop]
        for (uint probe = 0u; probe < 64u; ++probe) {
            const uint slot = (start + probe) & mask;
            const uint rangeIndex = gEditDeltaRangeTable[slot];
            if (rangeIndex == 0xFFFFFFFFu) {
                break;
            }
            if (rangeIndex < rangeCount &&
                all(gEditDeltaRanges[rangeIndex].brickCoord == brickCoord)) {
                matchedRangeIndex = rangeIndex;
                break;
            }
        }
    }

    if (matchedRangeIndex == 0xFFFFFFFFu) {
        [loop]
        for (uint rangeIndex = 0u; rangeIndex < rangeCount; ++rangeIndex) {
            const SparseEditDeltaRange range = gEditDeltaRanges[rangeIndex];
            if (all(range.brickCoord == brickCoord)) {
                matchedRangeIndex = rangeIndex;
                break;
            }
        }
    }

    if (matchedRangeIndex != 0xFFFFFFFFu) {
        const SparseEditDeltaRange range = gEditDeltaRanges[matchedRangeIndex];
        const uint firstDelta = min(range.firstDelta, deltaCount);
        const uint endDelta = min(firstDelta + range.deltaCount, deltaCount);
        [loop]
        for (uint i = firstDelta; i < endDelta; ++i) {
            const SparseEditDelta delta = gEditDeltas[i];
            if (delta.packedLocal == packedLocal &&
                delta.revision >= bestRevision) {
                voxel = delta.voxel;
                bestRevision = delta.revision;
                revision = delta.revision;
                found = true;
            }
        }
    }
    return found;
}

bool TrySampleSparseVoxelWithEditDeltas(
    int3 brickCoord,
    uint3 localVoxel,
    out uint voxel,
    out bool fromEditDelta,
    out uint editRevision)
{
    fromEditDelta = false;
    editRevision = 0u;
    if (TrySampleEditDelta(brickCoord, localVoxel, voxel, editRevision)) {
        fromEditDelta = true;
        return true;
    }
    return TrySampleResidentVoxel(brickCoord, localVoxel, voxel);
}

bool IsActivePhysicsMaterial(uint material, uint mask) {
    if (material == MAT_SAND) {
        return (mask & SPARSE_PHYSICS_MATERIAL_SAND) != 0u;
    }
    if (material == MAT_WATER) {
        return (mask & SPARSE_PHYSICS_MATERIAL_WATER) != 0u;
    }
    if (material == MAT_LAVA) {
        return (mask & SPARSE_PHYSICS_MATERIAL_LAVA) != 0u;
    }
    return false;
}

bool IsSparseFluidMaterial(uint material) {
    return material == MAT_WATER || material == MAT_LAVA;
}

void OffsetSparseLocal(
    int3 baseBrick,
    uint3 baseLocal,
    int3 offset,
    out int3 outBrick,
    out uint3 outLocal)
{
    outBrick = baseBrick;
    int3 local = int3(baseLocal) + offset;
    if (local.x < 0) {
        local.x += int(SPARSE_BRICK_SIZE);
        outBrick.x -= 1;
    } else if (local.x >= int(SPARSE_BRICK_SIZE)) {
        local.x -= int(SPARSE_BRICK_SIZE);
        outBrick.x += 1;
    }
    if (local.y < 0) {
        local.y += int(SPARSE_BRICK_SIZE);
        outBrick.y -= 1;
    } else if (local.y >= int(SPARSE_BRICK_SIZE)) {
        local.y -= int(SPARSE_BRICK_SIZE);
        outBrick.y += 1;
    }
    if (local.z < 0) {
        local.z += int(SPARSE_BRICK_SIZE);
        outBrick.z -= 1;
    } else if (local.z >= int(SPARSE_BRICK_SIZE)) {
        local.z -= int(SPARSE_BRICK_SIZE);
        outBrick.z += 1;
    }
    outLocal = uint3(local);
}

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint packetIndex = dispatchThreadId.x;
    if (packetIndex >= gConstants.packetCount) {
        return;
    }

    // GPU proposal path. This pass samples resident sparse bricks plus the
    // per-dispatch edit-delta snapshot and emits compact source/destination
    // proposals. The CPU apply step remains authoritative: it revalidates page
    // generation, edit revisions, generated terrain, persistent edits, and
    // same-batch conflicts before mutating sparse overlays.
    SparsePhysicsWorkPacket packet = gPackets[packetIndex];
    uint checksum =
        uint(packet.brickCoord.x) ^
        (uint(packet.brickCoord.y) * 1664525u) ^
        (uint(packet.brickCoord.z) * 1013904223u) ^
        packet.packedRegionMin ^
        packet.packedRegionMax ^
        packet.materialMask ^
        packet.priority ^
        packet.generation ^
        packet.expectedPageIndex ^
        packet.expectedPageGeneration ^
        gConstants.editDeltaCount ^
        gConstants.editDeltaRangeCount ^
        gConstants.editDeltaRangeTableCapacity ^
        gConstants.frameIndex ^
        gConstants.pageTableCapacity;

    uint ignored = 0;
    InterlockedAdd(gDiagnostics[0], 1u, ignored);
    InterlockedOr(gDiagnostics[1], packet.materialMask, ignored);
    InterlockedAdd(gDiagnostics[2], checksum, ignored);
    gDiagnostics[3] = gConstants.frameIndex;
    InterlockedMax(gDiagnostics[4], packet.priority, ignored);
    InterlockedXor(gDiagnostics[5], packet.generation, ignored);

    SparsePhysicsPacketResult result;
    result.brickCoord = packet.brickCoord;
    result.packetIndex = packetIndex;
    result.destinationBrickCoord = packet.brickCoord;
    result.destinationFlags = 0u;
    result.generation = packet.generation;
    result.materialMask = packet.materialMask;
    result.checksum = checksum;
    result.status = 1u;
    if (packet.expectedPageIndex != 0xFFFFFFFFu && packet.expectedPageGeneration != 0u) {
        result.status |= PACKET_STATUS_HAS_EXPECTED_PAGE;
        BrickPageEntry pageEntry;
        if (LookupSparseBrick(packet.brickCoord, pageEntry) &&
            pageEntry.pageIndex == packet.expectedPageIndex &&
            pageEntry.generation == packet.expectedPageGeneration) {
            result.status |= PACKET_STATUS_PAGE_MATCH;
        } else {
            result.status |= PACKET_STATUS_PAGE_STALE;
        }
    }
    result.expectedPageIndex = packet.expectedPageIndex;
    result.expectedPageGeneration = packet.expectedPageGeneration;
    result.packedSourceLocal = 0u;
    result.packedDestinationLocal = 0u;
    result.sourceVoxel = 0u;
    result.destinationVoxel = 0u;
    result.sourceRevision = 0u;
    result.destinationRevision = 0u;

    const uint3 regionMin = UnpackLocal(packet.packedRegionMin);
    const uint3 regionMax = min(UnpackLocal(packet.packedRegionMax), uint3(15u, 15u, 15u));
    bool missingBelow = false;
    bool foundProposal = false;
    [loop]
    for (uint z = regionMin.z; z <= regionMax.z && !foundProposal; ++z) {
        [loop]
        for (uint y = regionMax.y + 1u; y > regionMin.y && !foundProposal; --y) {
            const uint localY = y - 1u;
            [loop]
            for (uint x = regionMin.x; x <= regionMax.x; ++x) {
                const uint3 local = uint3(x, localY, z);
                uint voxel = 0u;
                bool sourceFromDelta = false;
                uint sourceRevision = 0u;
                if (!TrySampleSparseVoxelWithEditDeltas(packet.brickCoord, local, voxel, sourceFromDelta, sourceRevision)) {
                    continue;
                }
                if (!IsActivePhysicsMaterial(UnpackMaterial(voxel), packet.materialMask)) {
                    continue;
                }

                int3 belowBrick;
                uint3 belowLocal;
                OffsetSparseLocal(packet.brickCoord, local, int3(0, -1, 0), belowBrick, belowLocal);

                uint belowVoxel = 0u;
                bool belowFromDelta = false;
                uint belowRevision = 0u;
                if (!TrySampleSparseVoxelWithEditDeltas(belowBrick, belowLocal, belowVoxel, belowFromDelta, belowRevision)) {
                    missingBelow = true;
                    // Collision is CPU-authoritative. A missing render page
                    // is allowed to produce an optimistic fall proposal; the
                    // CPU apply step samples procedural terrain and persistent
                    // edits before committing the move. This keeps GPU packet
                    // physics from stalling forever on render residency.
                    belowVoxel = 0u;
                }
                if (UnpackMaterial(belowVoxel) == MAT_AIR) {
                    result.destinationBrickCoord = belowBrick;
                    result.destinationFlags = belowBrick.y != packet.brickCoord.y ? 1u : 0u;
                    result.packedSourceLocal = PackLocal(local);
                    result.packedDestinationLocal = PackLocal(belowLocal);
                    result.sourceVoxel = voxel;
                    result.destinationVoxel = belowVoxel;
                    result.sourceRevision = sourceRevision;
                    result.destinationRevision = belowRevision;
                    result.status |= PACKET_STATUS_PROPOSAL;
                    if (sourceFromDelta || belowFromDelta) {
                        result.status |= PACKET_STATUS_EDIT_DELTA_HIT;
                    }
                    foundProposal = true;
                    break;
                }

                const uint sourceMaterial = UnpackMaterial(voxel);
                if (IsSparseFluidMaterial(sourceMaterial)) {
                    const uint startDir = (checksum + local.x * 13u + local.z * 17u + gConstants.frameIndex) & 3u;
                    [unroll]
                    for (uint dirIndex = 0u; dirIndex < 4u; ++dirIndex) {
                        const uint dir = (startDir + dirIndex) & 3u;
                        int3 offset = int3(1, 0, 0);
                        if (dir == 1u) {
                            offset = int3(-1, 0, 0);
                        } else if (dir == 2u) {
                            offset = int3(0, 0, 1);
                        } else if (dir == 3u) {
                            offset = int3(0, 0, -1);
                        }

                        int3 lateralBrick;
                        uint3 lateralLocal;
                        OffsetSparseLocal(packet.brickCoord, local, offset, lateralBrick, lateralLocal);

                        uint lateralVoxel = 0u;
                        bool lateralFromDelta = false;
                        uint lateralRevision = 0u;
                        if (!TrySampleSparseVoxelWithEditDeltas(lateralBrick, lateralLocal, lateralVoxel, lateralFromDelta, lateralRevision)) {
                            missingBelow = true;
                            lateralVoxel = 0u;
                        }
                        if (UnpackMaterial(lateralVoxel) != MAT_AIR) {
                            continue;
                        }

                        result.destinationBrickCoord = lateralBrick;
                        result.destinationFlags = 2u | (any(lateralBrick != packet.brickCoord) ? 1u : 0u);
                        result.packedSourceLocal = PackLocal(local);
                        result.packedDestinationLocal = PackLocal(lateralLocal);
                        result.sourceVoxel = voxel;
                        result.destinationVoxel = lateralVoxel;
                        result.sourceRevision = sourceRevision;
                        result.destinationRevision = lateralRevision;
                        result.status |= PACKET_STATUS_PROPOSAL;
                        if (sourceFromDelta || lateralFromDelta) {
                            result.status |= PACKET_STATUS_EDIT_DELTA_HIT;
                        }
                        foundProposal = true;
                        break;
                    }
                    if (foundProposal) {
                        break;
                    }
                }
            }
        }
    }
    if (missingBelow && !foundProposal) {
        result.status |= PACKET_STATUS_MISSING_BELOW;
    }
    gResults[packetIndex] = result;
}
