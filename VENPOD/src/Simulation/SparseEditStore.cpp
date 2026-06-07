#include "SparseEditStore.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <limits>
#include <string>
#include <system_error>
#include <utility>

namespace VENPOD::Simulation {

uint32_t PackSparseEditLocal(LocalVoxelCoord local) {
    return (static_cast<uint32_t>(local.x) & 0xFFu) |
        ((static_cast<uint32_t>(local.y) & 0xFFu) << 8u) |
        ((static_cast<uint32_t>(local.z) & 0xFFu) << 16u);
}

LocalVoxelCoord UnpackSparseEditLocal(uint32_t packedLocal) {
    return LocalVoxelCoord{
        static_cast<uint8_t>(packedLocal & 0xFFu),
        static_cast<uint8_t>((packedLocal >> 8u) & 0xFFu),
        static_cast<uint8_t>((packedLocal >> 16u) & 0xFFu)
    };
}

bool IsSparseEditPersistencePathAllowed(const std::filesystem::path& path) {
    if (path.empty() || !path.has_filename()) {
        return false;
    }
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return extension == ".vsed";
}

namespace {

constexpr uint32_t kSparseEditFileMagic = 0x44455356u; // VSED, little-endian
constexpr uint32_t kSparseEditFileVersion = 1u;
constexpr uint64_t kSparseEditFileMaxOverlays = 1'000'000ull;
constexpr uint64_t kSparseEditFileMaxVoxels = 128'000'000ull;
constexpr uint64_t kSparseEditFileMaxBytes = 1024ull * 1024ull * 1024ull;
constexpr uint64_t kSparseEditFileHeaderBytes =
    sizeof(uint32_t) * 4ull + sizeof(uint64_t) * 2ull;
constexpr uint64_t kSparseEditFileOverlayBytes =
    sizeof(int32_t) * 3ull + sizeof(uint32_t) * 2ull;
constexpr uint64_t kSparseEditFileVoxelBytes =
    sizeof(uint16_t) + sizeof(uint32_t);

uint32_t PackSparseEditLocalIndex(uint16_t localIndex) {
    return PackSparseEditLocal(LocalVoxelFromIndex(localIndex));
}

uint32_t SaturatingSizeToUint32(size_t value) {
    if (value > static_cast<size_t>(std::numeric_limits<uint32_t>::max())) {
        return std::numeric_limits<uint32_t>::max();
    }
    return static_cast<uint32_t>(value);
}

bool BeginOverlayRevisionUpdate(
    BrickEditOverlay& overlay,
    std::vector<SparseEditDelta>& pendingGpuDeltas)
{
    if (overlay.revision != std::numeric_limits<uint32_t>::max()) {
        return false;
    }

    pendingGpuDeltas.erase(
        std::remove_if(
            pendingGpuDeltas.begin(),
            pendingGpuDeltas.end(),
            [&overlay](const SparseEditDelta& delta) {
                return delta.coord == overlay.coord;
            }),
        pendingGpuDeltas.end());
    overlay.revision = 0;
    return true;
}

template <typename T>
bool WriteBinary(std::ostream& stream, const T& value) {
    stream.write(reinterpret_cast<const char*>(&value), sizeof(T));
    return stream.good();
}

template <typename T>
bool ReadBinary(std::istream& stream, T* value) {
    if (!value) {
        return false;
    }
    stream.read(reinterpret_cast<char*>(value), sizeof(T));
    return stream.good();
}

bool AddUint64(uint64_t a, uint64_t b, uint64_t* out) {
    if (!out || a > std::numeric_limits<uint64_t>::max() - b) {
        return false;
    }
    *out = a + b;
    return true;
}

bool MulUint64(uint64_t a, uint64_t b, uint64_t* out) {
    if (!out || (b != 0 && a > std::numeric_limits<uint64_t>::max() / b)) {
        return false;
    }
    *out = a * b;
    return true;
}

bool TryGetInputStreamSize(std::ifstream& stream, uint64_t* outSize) {
    if (!outSize) {
        return false;
    }
    const std::streampos end = stream.tellg();
    const std::streamoff signedSize = end;
    if (signedSize < 0) {
        return false;
    }
    *outSize = static_cast<uint64_t>(signedSize);
    stream.seekg(0, std::ios::beg);
    return static_cast<bool>(stream);
}

bool ComputeSparseEditFileBytes(uint64_t overlayCount, uint64_t voxelCount, uint64_t* outBytes) {
    uint64_t overlayBytes = 0;
    uint64_t voxelBytes = 0;
    uint64_t payloadBytes = 0;
    if (!MulUint64(overlayCount, kSparseEditFileOverlayBytes, &overlayBytes) ||
        !MulUint64(voxelCount, kSparseEditFileVoxelBytes, &voxelBytes) ||
        !AddUint64(overlayBytes, voxelBytes, &payloadBytes) ||
        !AddUint64(kSparseEditFileHeaderBytes, payloadBytes, outBytes)) {
        return false;
    }
    return true;
}

}

SparseEditDeltaBatch BuildSparseEditDeltaBatch(
    const std::vector<SparseEditDelta>& deltas,
    uint32_t maxDeltas,
    uint32_t maxRanges,
    uint32_t rangeTableCapacity)
{
    SparseEditDeltaBatch batch;
    if (deltas.empty() || maxDeltas == 0 || maxRanges == 0) {
        batch.overflow = !deltas.empty();
        batch.truncated = !deltas.empty();
        return batch;
    }

    if (rangeTableCapacity != 0 && (rangeTableCapacity & (rangeTableCapacity - 1u)) != 0) {
        batch.overflow = true;
        batch.truncated = true;
        return batch;
    }
    batch.rangeTableCapacity = rangeTableCapacity;

    std::vector<SparseEditDelta> sorted(
        deltas.begin(),
        deltas.end());
    const bool coalesceBeforeCap = deltas.size() > maxDeltas;
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) {
        if (a.coord != b.coord) {
            return a.coord < b.coord;
        }
        if (a.packedLocal != b.packedLocal) {
            return a.packedLocal < b.packedLocal;
        }
        return a.revision > b.revision;
    });

    if (coalesceBeforeCap) {
        std::vector<SparseEditDelta> latestUnique;
        latestUnique.reserve(sorted.size());
        for (const SparseEditDelta& delta : sorted) {
            if (!latestUnique.empty() &&
                latestUnique.back().coord == delta.coord &&
                latestUnique.back().packedLocal == delta.packedLocal) {
                continue;
            }
            latestUnique.push_back(delta);
        }
        sorted.swap(latestUnique);
    }

    batch.inputDeltaCount = std::min<uint32_t>(SaturatingSizeToUint32(sorted.size()), maxDeltas);
    batch.truncated = batch.inputDeltaCount < sorted.size();
    batch.overflow = coalesceBeforeCap || batch.truncated;
    sorted.resize(batch.inputDeltaCount);
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) {
        if (a.coord != b.coord) {
            return a.coord < b.coord;
        }
        if (a.packedLocal != b.packedLocal) {
            return a.packedLocal < b.packedLocal;
        }
        return a.revision < b.revision;
    });

    batch.deltas.reserve(sorted.size());
    batch.ranges.reserve(std::min<uint32_t>(maxRanges, batch.inputDeltaCount));
    for (size_t i = 0; i < sorted.size();) {
        const BrickCoord coord = sorted[i].coord;
        const uint32_t firstDelta = static_cast<uint32_t>(batch.deltas.size());
        uint32_t latestRevision = 0;
        size_t j = i;
        while (j < sorted.size() && sorted[j].coord == coord) {
            latestRevision = std::max(latestRevision, sorted[j].revision);
            ++j;
        }

        if (batch.ranges.size() >= maxRanges) {
            batch.overflow = true;
            batch.truncated = true;
            break;
        }

        batch.deltas.insert(
            batch.deltas.end(),
            sorted.begin() + static_cast<std::ptrdiff_t>(i),
            sorted.begin() + static_cast<std::ptrdiff_t>(j));
        batch.ranges.push_back({
            coord,
            firstDelta,
            static_cast<uint32_t>(j - i),
            latestRevision
        });
        i = j;
    }

    if (rangeTableCapacity != 0) {
        constexpr uint32_t kInvalidRange = 0xFFFFFFFFu;
        batch.rangeTable.assign(rangeTableCapacity, kInvalidRange);
        const uint32_t mask = rangeTableCapacity - 1u;
        for (uint32_t rangeIndex = 0; rangeIndex < batch.ranges.size(); ++rangeIndex) {
            const uint32_t start = HashBrickCoord32(batch.ranges[rangeIndex].coord) & mask;
            bool inserted = false;
            for (uint32_t probe = 0; probe < 64u; ++probe) {
                const uint32_t slot = (start + probe) & mask;
                if (batch.rangeTable[slot] == kInvalidRange) {
                    batch.rangeTable[slot] = rangeIndex;
                    inserted = true;
                    break;
                }
            }
            if (!inserted) {
                batch.overflow = true;
                batch.truncated = true;
                break;
            }
        }
    }

    return batch;
}

void SparseEditStore::SetVoxel(int32_t worldX, int32_t worldY, int32_t worldZ, uint32_t packedVoxel) {
    const BrickCoord brick = BrickCoord::FromWorldVoxel(worldX, worldY, worldZ);
    const LocalVoxelCoord local = LocalVoxelFromWorld(worldX, worldY, worldZ);
    const uint16_t localIndex = LocalVoxelIndex(local);

    BrickEditOverlay& overlay = m_overlays[brick];
    if (overlay.voxels.empty()) {
        overlay.coord = brick;
    }

    const bool resetRevisionEpoch = BeginOverlayRevisionUpdate(overlay, m_pendingGpuDeltas);
    const bool inserted = overlay.voxels.find(localIndex) == overlay.voxels.end();
    overlay.voxels[localIndex] = packedVoxel;
    const uint32_t revision = ++overlay.revision;
    overlay.dirtyDisk = true;
    overlay.dirtyGpu = true;
    if (resetRevisionEpoch) {
        m_pendingGpuDeltas.reserve(m_pendingGpuDeltas.size() + overlay.voxels.size());
        for (const auto& [overlayLocalIndex, overlayVoxel] : overlay.voxels) {
            m_pendingGpuDeltas.push_back({
                brick,
                PackSparseEditLocalIndex(overlayLocalIndex),
                overlayVoxel,
                revision
            });
        }
    } else {
        m_pendingGpuDeltas.push_back({
            brick,
            PackSparseEditLocal(local),
            packedVoxel,
            revision
        });
    }
    if (inserted) {
        ++m_editedVoxelCount;
    }
    ++m_revisionSerial;
}

bool SparseEditStore::TryGetVoxel(
    int32_t worldX,
    int32_t worldY,
    int32_t worldZ,
    uint32_t* outVoxel) const
{
    const BrickCoord brick = BrickCoord::FromWorldVoxel(worldX, worldY, worldZ);
    auto overlayIt = m_overlays.find(brick);
    if (overlayIt == m_overlays.end()) {
        return false;
    }

    const uint16_t localIndex = LocalVoxelIndex(LocalVoxelFromWorld(worldX, worldY, worldZ));
    auto voxelIt = overlayIt->second.voxels.find(localIndex);
    if (voxelIt == overlayIt->second.voxels.end()) {
        return false;
    }

    if (outVoxel) {
        *outVoxel = voxelIt->second;
    }
    return true;
}

bool SparseEditStore::HasOverlay(const BrickCoord& coord) const {
    return m_overlays.find(coord) != m_overlays.end();
}

void SparseEditStore::ForEachVoxelInBrick(
    const BrickCoord& coord,
    const std::function<void(uint16_t localIndex, uint32_t packedVoxel)>& visitor) const
{
    auto overlayIt = m_overlays.find(coord);
    if (overlayIt == m_overlays.end() || !visitor) {
        return;
    }

    for (const auto& [localIndex, packedVoxel] : overlayIt->second.voxels) {
        visitor(localIndex, packedVoxel);
    }
}

void SparseEditStore::ForEachOverlay(
    const std::function<void(const BrickEditOverlay& overlay)>& visitor) const
{
    if (!visitor) {
        return;
    }

    for (const auto& [coord, overlay] : m_overlays) {
        (void)coord;
        visitor(overlay);
    }
}

void SparseEditStore::ApplyToGeneratedBrick(GeneratedSparseBrick& brick) const {
    auto overlayIt = m_overlays.find(brick.coord);
    if (overlayIt == m_overlays.end()) {
        return;
    }

    for (const auto& [localIndex, packedVoxel] : overlayIt->second.voxels) {
        if (localIndex < brick.voxels.size()) {
            brick.voxels[localIndex] = packedVoxel;
        }
    }

    SparseTerrainGenerator::ComputeOccupancyAndFlags(brick);
}

std::vector<SparseEditDelta> SparseEditStore::BuildDeltaSnapshotForBricks(
    const std::vector<BrickCoord>& coords,
    uint32_t maxDeltas) const
{
    std::vector<SparseEditDelta> snapshot;
    if (coords.empty() || maxDeltas == 0) {
        return snapshot;
    }

    snapshot.reserve(std::min<uint32_t>(maxDeltas, 256u));
    for (const BrickCoord& coord : coords) {
        auto overlayIt = m_overlays.find(coord);
        if (overlayIt == m_overlays.end()) {
            continue;
        }

        const BrickEditOverlay& overlay = overlayIt->second;
        for (const auto& [localIndex, packedVoxel] : overlay.voxels) {
            if (snapshot.size() >= maxDeltas) {
                return snapshot;
            }
            snapshot.push_back({
                coord,
                PackSparseEditLocalIndex(localIndex),
                packedVoxel,
                overlay.revision
            });
        }
    }
    return snapshot;
}

uint32_t SparseEditStore::GetOverlayRevision(const BrickCoord& coord) const {
    auto overlayIt = m_overlays.find(coord);
    if (overlayIt == m_overlays.end()) {
        return 0;
    }
    return overlayIt->second.revision;
}

bool SparseEditStore::SaveToFile(const std::filesystem::path& path) {
    if (!IsSparseEditPersistencePathAllowed(path)) {
        return false;
    }

    const std::filesystem::path parent = path.parent_path();
    if (!parent.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            return false;
        }
    }

    std::vector<const BrickEditOverlay*> overlays;
    overlays.reserve(m_overlays.size());
    uint64_t totalVoxels = 0;
    for (const auto& [coord, overlay] : m_overlays) {
        (void)coord;
        if (overlay.voxels.empty()) {
            continue;
        }
        overlays.push_back(&overlay);
        totalVoxels += static_cast<uint64_t>(overlay.voxels.size());
        if (totalVoxels > kSparseEditFileMaxVoxels) {
            return false;
        }
    }

    std::sort(overlays.begin(), overlays.end(), [](const auto* a, const auto* b) {
        return a->coord < b->coord;
    });

    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        return false;
    }

    const uint32_t magic = kSparseEditFileMagic;
    const uint32_t version = kSparseEditFileVersion;
    const uint32_t brickSize = SPARSE_BRICK_SIZE;
    const uint32_t reserved = 0;
    const uint64_t overlayCount = static_cast<uint64_t>(overlays.size());
    if (!WriteBinary(stream, magic) ||
        !WriteBinary(stream, version) ||
        !WriteBinary(stream, brickSize) ||
        !WriteBinary(stream, reserved) ||
        !WriteBinary(stream, overlayCount) ||
        !WriteBinary(stream, totalVoxels)) {
        return false;
    }

    for (const BrickEditOverlay* overlay : overlays) {
        const uint32_t voxelCount = static_cast<uint32_t>(overlay->voxels.size());
        if (!WriteBinary(stream, overlay->coord.x) ||
            !WriteBinary(stream, overlay->coord.y) ||
            !WriteBinary(stream, overlay->coord.z) ||
            !WriteBinary(stream, overlay->revision) ||
            !WriteBinary(stream, voxelCount)) {
            return false;
        }

        std::vector<std::pair<uint16_t, uint32_t>> voxels(
            overlay->voxels.begin(),
            overlay->voxels.end());
        std::sort(voxels.begin(), voxels.end(), [](const auto& a, const auto& b) {
            return a.first < b.first;
        });
        for (const auto& [localIndex, packedVoxel] : voxels) {
            if (!WriteBinary(stream, localIndex) || !WriteBinary(stream, packedVoxel)) {
                return false;
            }
        }
    }

    stream.flush();
    if (!stream.good()) {
        return false;
    }

    for (auto& [coord, overlay] : m_overlays) {
        (void)coord;
        overlay.dirtyDisk = false;
    }
    return true;
}

bool SparseEditStore::LoadFromFile(const std::filesystem::path& path) {
    if (!IsSparseEditPersistencePathAllowed(path)) {
        return false;
    }

    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) {
        return false;
    }
    uint64_t fileBytes = 0;
    if (!TryGetInputStreamSize(stream, &fileBytes) ||
        fileBytes < kSparseEditFileHeaderBytes ||
        fileBytes > kSparseEditFileMaxBytes) {
        return false;
    }

    uint32_t magic = 0;
    uint32_t version = 0;
    uint32_t brickSize = 0;
    uint32_t reserved = 0;
    uint64_t overlayCount = 0;
    uint64_t totalVoxelCount = 0;
    if (!ReadBinary(stream, &magic) ||
        !ReadBinary(stream, &version) ||
        !ReadBinary(stream, &brickSize) ||
        !ReadBinary(stream, &reserved) ||
        !ReadBinary(stream, &overlayCount) ||
        !ReadBinary(stream, &totalVoxelCount)) {
        return false;
    }

    if (magic != kSparseEditFileMagic ||
        version != kSparseEditFileVersion ||
        brickSize != SPARSE_BRICK_SIZE ||
        reserved != 0 ||
        overlayCount > kSparseEditFileMaxOverlays ||
        totalVoxelCount > kSparseEditFileMaxVoxels) {
        return false;
    }
    uint64_t expectedBytes = 0;
    if (!ComputeSparseEditFileBytes(overlayCount, totalVoxelCount, &expectedBytes) ||
        expectedBytes != fileBytes) {
        return false;
    }

    std::unordered_map<BrickCoord, BrickEditOverlay, BrickCoordHash> loaded;
    loaded.reserve(static_cast<size_t>(std::min<uint64_t>(overlayCount, 65536ull)));
    uint64_t readVoxelCount = 0;

    for (uint64_t overlayIndex = 0; overlayIndex < overlayCount; ++overlayIndex) {
        BrickEditOverlay overlay;
        uint32_t voxelCount = 0;
        if (!ReadBinary(stream, &overlay.coord.x) ||
            !ReadBinary(stream, &overlay.coord.y) ||
            !ReadBinary(stream, &overlay.coord.z) ||
            !ReadBinary(stream, &overlay.revision) ||
            !ReadBinary(stream, &voxelCount)) {
            return false;
        }

        if (voxelCount == 0 ||
            overlay.revision == 0 ||
            voxelCount > SPARSE_BRICK_VOXEL_COUNT ||
            readVoxelCount + voxelCount > totalVoxelCount) {
            return false;
        }

        overlay.voxels.reserve(voxelCount);
        for (uint32_t voxelIndex = 0; voxelIndex < voxelCount; ++voxelIndex) {
            uint16_t localIndex = 0;
            uint32_t packedVoxel = 0;
            if (!ReadBinary(stream, &localIndex) || !ReadBinary(stream, &packedVoxel)) {
                return false;
            }
            if (localIndex >= SPARSE_BRICK_VOXEL_COUNT) {
                return false;
            }
            if (!overlay.voxels.emplace(localIndex, packedVoxel).second) {
                return false;
            }
        }
        readVoxelCount += voxelCount;

        overlay.dirtyDisk = false;
        overlay.dirtyGpu = false;
        if (!loaded.emplace(overlay.coord, std::move(overlay)).second) {
            return false;
        }
    }

    if (readVoxelCount != totalVoxelCount) {
        return false;
    }

    char trailing = 0;
    if (stream.read(&trailing, 1)) {
        return false;
    }
    if (!stream.eof()) {
        return false;
    }

    size_t editedVoxelCount = 0;
    for (const auto& [coord, overlay] : loaded) {
        (void)coord;
        editedVoxelCount += overlay.voxels.size();
    }

    m_overlays = std::move(loaded);
    m_pendingGpuDeltas.clear();
    m_editedVoxelCount = editedVoxelCount;
    ++m_revisionSerial;
    return true;
}

void SparseEditStore::ClearPendingGpuDeltas(uint32_t consumedCount) {
    if (consumedCount == 0) {
        return;
    }
    if (consumedCount >= m_pendingGpuDeltas.size()) {
        m_pendingGpuDeltas.clear();
        return;
    }
    m_pendingGpuDeltas.erase(
        m_pendingGpuDeltas.begin(),
        m_pendingGpuDeltas.begin() + static_cast<std::ptrdiff_t>(consumedCount));
}

void SparseEditStore::ClearPendingGpuDeltas() {
    m_pendingGpuDeltas.clear();
}

} // namespace VENPOD::Simulation
