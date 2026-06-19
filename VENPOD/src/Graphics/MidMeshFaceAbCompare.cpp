#include "MidMeshFaceAbCompare.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>

namespace VENPOD::Graphics {

namespace {

using Simulation::SparseSurfaceFace;

// Canonical face ordering: lexicographic memcmp over the raw 16-byte payload
// (worldX/worldY/worldZ/payload, in declaration order). This is a TOTAL order
// over the exact bytes, so equal faces are adjacent and duplicates are kept -
// the property a multiset compare needs. SparseSurfaceFace is a trivially
// copyable POD of four 32-bit fields with no padding (static_assert size==16),
// so memcmp is well defined.
bool FaceLess(const SparseSurfaceFace& a, const SparseSurfaceFace& b) {
    return std::memcmp(&a, &b, sizeof(SparseSurfaceFace)) < 0;
}

bool FaceEqual(const SparseSurfaceFace& a, const SparseSurfaceFace& b) {
    return std::memcmp(&a, &b, sizeof(SparseSurfaceFace)) == 0;
}

// Count distinct faces in an already-sorted vector.
uint32_t CountUnique(const std::vector<SparseSurfaceFace>& sorted) {
    uint32_t unique = 0;
    for (size_t i = 0; i < sorted.size();) {
        size_t j = i + 1;
        while (j < sorted.size() && FaceEqual(sorted[j], sorted[i])) {
            ++j;
        }
        ++unique;
        i = j;
    }
    return unique;
}

} // namespace

MidMeshFaceAbResult CompareMidMeshFacesMultiset(
    const std::vector<SparseSurfaceFace>& gpuFaces,
    const std::vector<SparseSurfaceFace>& cpuFaces,
    MidMeshFaceAbMode mode,
    uint32_t overflowStatus,
    uint32_t maxSamples)
{
    static_assert(sizeof(SparseSurfaceFace) == 16,
        "multiset canonicalization assumes a tight 16-byte SparseSurfaceFace");

    MidMeshFaceAbResult result;
    result.mode = mode;
    result.overflow = overflowStatus;
    result.gpuFaceCount = static_cast<uint32_t>(gpuFaces.size());
    result.cpuFaceCount = static_cast<uint32_t>(cpuFaces.size());

    // Canonicalize: sort copies by the raw bytes, KEEPING duplicates.
    std::vector<SparseSurfaceFace> gpu = gpuFaces;
    std::vector<SparseSurfaceFace> cpu = cpuFaces;
    std::sort(gpu.begin(), gpu.end(), FaceLess);
    std::sort(cpu.begin(), cpu.end(), FaceLess);

    result.gpuUniqueFaceCount = CountUnique(gpu);
    result.cpuUniqueFaceCount = CountUnique(cpu);

    // Merge-walk the two sorted multisets group by group. For each distinct face
    // we know its multiplicity on each side; the per-mode rules then decide
    // missing/extra/multiplicity-mismatch.
    size_t gi = 0;
    size_t ci = 0;
    const size_t gn = gpu.size();
    const size_t cn = cpu.size();

    auto pushSample = [maxSamples](std::vector<MidMeshFaceAbDiff>& out,
        const SparseSurfaceFace& face, uint32_t gpuCount, uint32_t cpuCount) {
        if (out.size() < maxSamples) {
            MidMeshFaceAbDiff diff;
            diff.face = face;
            diff.gpuCount = gpuCount;
            diff.cpuCount = cpuCount;
            out.push_back(diff);
        }
    };

    while (gi < gn || ci < cn) {
        // Decide which distinct face is next in canonical order, and tally its
        // multiplicity on each side.
        const SparseSurfaceFace* gFace = (gi < gn) ? &gpu[gi] : nullptr;
        const SparseSurfaceFace* cFace = (ci < cn) ? &cpu[ci] : nullptr;

        int cmp;
        if (gFace && cFace) {
            cmp = std::memcmp(gFace, cFace, sizeof(SparseSurfaceFace));
        } else if (gFace) {
            cmp = -1; // only GPU has faces left
        } else {
            cmp = 1;  // only CPU has faces left
        }

        if (cmp < 0) {
            // GPU-only face. Count its run.
            const SparseSurfaceFace face = *gFace;
            uint32_t gpuCount = 0;
            while (gi < gn && FaceEqual(gpu[gi], face)) { ++gi; ++gpuCount; }
            // Extra: GPU emitted a face the CPU does not have at all.
            result.extraGpuFaces += gpuCount;
            pushSample(result.sampleExtra, face, gpuCount, 0u);
        } else if (cmp > 0) {
            // CPU-only face. Count its run.
            const SparseSurfaceFace face = *cFace;
            uint32_t cpuCount = 0;
            while (ci < cn && FaceEqual(cpu[ci], face)) { ++ci; ++cpuCount; }
            // Missing: CPU has it, GPU has none.
            result.missingCpuFaces += cpuCount;
            pushSample(result.sampleMissing, face, 0u, cpuCount);
        } else {
            // Shared face. Count both runs.
            const SparseSurfaceFace face = *gFace;
            uint32_t gpuCount = 0;
            uint32_t cpuCount = 0;
            while (gi < gn && FaceEqual(gpu[gi], face)) { ++gi; ++gpuCount; }
            while (ci < cn && FaceEqual(cpu[ci], face)) { ++ci; ++cpuCount; }
            if (gpuCount > cpuCount) {
                // GPU emitted the same valid face more times than the CPU has it.
                const uint32_t extra = gpuCount - cpuCount;
                result.extraGpuFaces += extra;
                result.multiplicityMismatches += 1;
                pushSample(result.sampleExtra, face, gpuCount, cpuCount);
            } else if (gpuCount < cpuCount) {
                // GPU under-counts a face the CPU has more of.
                const uint32_t missing = cpuCount - gpuCount;
                result.missingCpuFaces += missing;
                result.multiplicityMismatches += 1;
                pushSample(result.sampleMissing, face, gpuCount, cpuCount);
            }
            // gpuCount == cpuCount -> exact, no diff.
        }
    }

    // Per-mode verdict.
    //   Equal:       no missing, no extra, no multiplicity mismatch.
    //   Containment: GPU subset of CPU -> NO extra and NO multiplicity-driven
    //                over-count (extraGpuFaces==0). Missing CPU faces are allowed.
    // A non-zero GPU overflow status fails either mode (the GPU could not emit
    // its full reserved range, so the comparison is not trustworthy).
    if (overflowStatus != 0u) {
        result.match = false;
    } else if (mode == MidMeshFaceAbMode::Equal) {
        result.match = (result.missingCpuFaces == 0u) &&
                       (result.extraGpuFaces == 0u) &&
                       (result.multiplicityMismatches == 0u);
    } else { // Containment
        result.match = (result.extraGpuFaces == 0u);
    }
    return result;
}

bool LogMidMeshFaceAbResult(const char* label, const MidMeshFaceAbResult& result) {
    const char* modeStr =
        (result.mode == MidMeshFaceAbMode::Equal) ? "equal" : "containment";
    spdlog::info(
        "AB_VERIFY label={} mode={} match={} gpuFaceCount={} gpuUniqueFaceCount={} "
        "cpuFaceCount={} cpuUniqueFaceCount={} missingCpuFaces={} extraGpuFaces={} "
        "multiplicityMismatches={} overflow={}",
        label ? label : "?", modeStr, result.match ? 1 : 0,
        result.gpuFaceCount, result.gpuUniqueFaceCount,
        result.cpuFaceCount, result.cpuUniqueFaceCount,
        result.missingCpuFaces, result.extraGpuFaces,
        result.multiplicityMismatches, result.overflow);

    if (!result.match) {
        for (const auto& d : result.sampleMissing) {
            const uint32_t p = d.face.payload;
            spdlog::warn(
                "[AB_VERIFY] {} MISSING (in CPU not GPU) face=({},{},{}) "
                "dir={} w={} h={} voxel={} gpuCount={} cpuCount={}",
                label ? label : "?", d.face.worldX, d.face.worldY, d.face.worldZ,
                Simulation::SparseSurfacePayloadDirection(p),
                Simulation::SparseSurfacePayloadWidth(p),
                Simulation::SparseSurfacePayloadHeight(p),
                Simulation::SparseSurfacePayloadVoxel(p),
                d.gpuCount, d.cpuCount);
        }
        for (const auto& d : result.sampleExtra) {
            const uint32_t p = d.face.payload;
            spdlog::warn(
                "[AB_VERIFY] {} EXTRA (in GPU not CPU) face=({},{},{}) "
                "dir={} w={} h={} voxel={} gpuCount={} cpuCount={}",
                label ? label : "?", d.face.worldX, d.face.worldY, d.face.worldZ,
                Simulation::SparseSurfacePayloadDirection(p),
                Simulation::SparseSurfacePayloadWidth(p),
                Simulation::SparseSurfacePayloadHeight(p),
                Simulation::SparseSurfacePayloadVoxel(p),
                d.gpuCount, d.cpuCount);
        }
    }
    return result.match;
}

bool RunMidMeshFaceAbSelfTest() {
    using Simulation::PackSparseSurfacePayload;

    // A small, fixed CPU reference set: 3 distinct top faces.
    auto makeFace = [](int32_t x, int32_t y, int32_t z, uint32_t voxel) {
        SparseSurfaceFace f;
        f.worldX = x;
        f.worldY = y;
        f.worldZ = z;
        f.payload = PackSparseSurfacePayload(3u, voxel, 1u, 1u);
        return f;
    };
    const SparseSurfaceFace fA = makeFace(0, 10, 0, 0u);
    const SparseSurfaceFace fB = makeFace(8, 11, 0, 1u);
    const SparseSurfaceFace fC = makeFace(0, 12, 8, 2u);

    const std::vector<SparseSurfaceFace> cpu = { fA, fB, fC };

    bool allOk = true;

    // --- Case 1: GPU has a DUPLICATE of a valid face (fA twice) and is MISSING fC.
    // A plain hash-SET would silently pass the duplicate; a true multiset must
    // flag the over-count as extra/multiplicityMismatch AND flag fC as missing.
    {
        const std::vector<SparseSurfaceFace> gpuDup = { fA, fA, fB }; // dup fA, no fC
        const MidMeshFaceAbResult r =
            CompareMidMeshFacesMultiset(gpuDup, cpu, MidMeshFaceAbMode::Equal);
        const bool ok =
            !r.match &&                       // must fail
            r.extraGpuFaces == 1u &&          // one extra copy of fA
            r.multiplicityMismatches == 1u && // fA: gpu 2 vs cpu 1
            r.missingCpuFaces == 1u;          // fC absent from GPU
        spdlog::info(
            "AB_SELFTEST case=dup_and_missing pass={} match={} extraGpuFaces={} "
            "multiplicityMismatches={} missingCpuFaces={} (expect match=0 extra=1 "
            "mult=1 missing=1)",
            ok ? 1 : 0, r.match ? 1 : 0, r.extraGpuFaces,
            r.multiplicityMismatches, r.missingCpuFaces);
        allOk = allOk && ok;
    }

    // --- Case 2: exact-equal GPU must PASS (proves the harness is not failing
    // everything). Order is shuffled to prove order-independence after canonical sort.
    {
        const std::vector<SparseSurfaceFace> gpuEq = { fC, fA, fB };
        const MidMeshFaceAbResult r =
            CompareMidMeshFacesMultiset(gpuEq, cpu, MidMeshFaceAbMode::Equal);
        const bool ok = r.match && r.extraGpuFaces == 0u &&
                        r.missingCpuFaces == 0u && r.multiplicityMismatches == 0u;
        spdlog::info(
            "AB_SELFTEST case=exact_equal pass={} match={} (expect match=1, all zero)",
            ok ? 1 : 0, r.match ? 1 : 0);
        allOk = allOk && ok;
    }

    // --- Case 3: Containment - a GPU SUBSET (fA only) must PASS containment but
    // FAIL equality, and a GPU with an EXTRA face (not in CPU) must FAIL both.
    {
        const std::vector<SparseSurfaceFace> gpuSubset = { fA };
        const MidMeshFaceAbResult rc =
            CompareMidMeshFacesMultiset(gpuSubset, cpu, MidMeshFaceAbMode::Containment);
        const MidMeshFaceAbResult re =
            CompareMidMeshFacesMultiset(gpuSubset, cpu, MidMeshFaceAbMode::Equal);
        const SparseSurfaceFace fExtra = makeFace(99, 99, 99, 7u);
        const std::vector<SparseSurfaceFace> gpuExtra = { fA, fB, fC, fExtra };
        const MidMeshFaceAbResult rx =
            CompareMidMeshFacesMultiset(gpuExtra, cpu, MidMeshFaceAbMode::Containment);
        const bool ok =
            rc.match &&                // subset passes containment
            rc.missingCpuFaces == 2u && // fB, fC missing (allowed in containment)
            !re.match &&               // same subset fails equality
            !rx.match &&               // an out-of-CPU extra fails containment
            rx.extraGpuFaces == 1u;
        spdlog::info(
            "AB_SELFTEST case=containment pass={} subsetContain={} subsetEqual={} "
            "extraContain={} (expect subsetContain=1 subsetEqual=0 extraContain=0)",
            ok ? 1 : 0, rc.match ? 1 : 0, re.match ? 1 : 0, rx.match ? 1 : 0);
        allOk = allOk && ok;
    }

    spdlog::info("AB_SELFTEST overall pass={} (multiset/multiplicity/containment logic {})",
        allOk ? 1 : 0, allOk ? "BITES" : "FAILED");
    return allOk;
}

} // namespace VENPOD::Graphics
