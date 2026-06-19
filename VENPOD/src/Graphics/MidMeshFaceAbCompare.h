#pragma once

// =============================================================================
// VENPOD GPU mid-mesh extraction - Phase B1.3.0 A/B VALIDATION HARNESS
// =============================================================================
// A reusable, DEBUG-ONLY comparison that takes the GPU-extracted faces (read
// back from the device) and the CPU reference faces for the SAME controlled
// tile and compares them as MULTISETS with multiplicity preserved - NOT a plain
// hash-set. This is the validation foundation every later B1.3a/b increment
// A/Bs against; it does NOT change production geometry and never touches the CPU
// extractor.
//
// Canonicalization: SORT the raw 16-byte SparseSurfaceFace payloads
// (lexicographic memcmp over worldX/worldY/worldZ/payload), KEEPING duplicates.
// A GPU output containing the same valid face twice MUST fail if the CPU has it
// once. Correctness rests on an EXACT sorted-vector compare; an order-independent
// hash is only a cheap pre-screen.
//
// Two modes (the caller picks):
//   * Equal       - GPU multiset == CPU multiset (final-increment equality).
//   * Containment - GPU multiset is a SUB-multiset of CPU (every GPU face appears
//                   in CPU with at least the GPU's multiplicity). For the
//                   subset-convergence increments where the GPU emits a partial
//                   but-correct subset of the CPU mesh.
// =============================================================================

#include "Simulation/SparseSurfaceExtractor.h"

#include <cstdint>
#include <vector>

namespace VENPOD::Graphics {

enum class MidMeshFaceAbMode : uint32_t {
    Equal = 0,       // GPU multiset must exactly equal CPU multiset.
    Containment = 1, // GPU multiset must be a sub-multiset of CPU multiset.
};

// One reported difference (a face present in one side with a multiplicity the
// comparison rejects). Carries the raw face so the caller can log decoded fields.
struct MidMeshFaceAbDiff {
    Simulation::SparseSurfaceFace face{};
    uint32_t gpuCount = 0;   // multiplicity in the GPU multiset
    uint32_t cpuCount = 0;   // multiplicity in the CPU multiset
};

struct MidMeshFaceAbResult {
    bool match = false;              // passes for the requested mode
    MidMeshFaceAbMode mode = MidMeshFaceAbMode::Equal;

    uint32_t gpuFaceCount = 0;       // total GPU faces (with duplicates)
    uint32_t gpuUniqueFaceCount = 0; // distinct GPU faces
    uint32_t cpuFaceCount = 0;       // total CPU faces (with duplicates)
    uint32_t cpuUniqueFaceCount = 0; // distinct CPU faces

    // Faces in CPU but NOT in GPU (or with lower GPU multiplicity). For
    // Containment these are EXPECTED (GPU is allowed to be a subset) and do NOT
    // by themselves fail the result.
    uint32_t missingCpuFaces = 0;
    // Faces in GPU but NOT in CPU (or with higher GPU multiplicity than CPU has).
    // Always a failure (the GPU emitted something the CPU never would, or emitted
    // a valid face too many times).
    uint32_t extraGpuFaces = 0;
    // Same face, different count, where the discrepancy is not already counted as
    // a pure missing/extra (i.e. both sides have the face but counts differ).
    uint32_t multiplicityMismatches = 0;

    uint32_t overflow = 0; // pass-through of the GPU status/overflow flag.

    // The first few concrete missing / extra faces for a readable mismatch log.
    std::vector<MidMeshFaceAbDiff> sampleMissing;
    std::vector<MidMeshFaceAbDiff> sampleExtra;
};

// Compare GPU faces vs CPU reference faces as multisets. `maxSamples` bounds how
// many concrete missing/extra faces are captured for logging (counts are always
// exact). `overflowStatus` is the GPU-written status flag, passed through to the
// result (a non-zero status forces a failure).
MidMeshFaceAbResult CompareMidMeshFacesMultiset(
    const std::vector<Simulation::SparseSurfaceFace>& gpuFaces,
    const std::vector<Simulation::SparseSurfaceFace>& cpuFaces,
    MidMeshFaceAbMode mode,
    uint32_t overflowStatus = 0u,
    uint32_t maxSamples = 4u);

// Emit the canonical AB_VERIFY log line (+ the first few decoded missing/extra
// faces on any mismatch). `label` lets the caller tag the source (e.g. "smoke").
// Returns result.match for convenience.
bool LogMidMeshFaceAbResult(const char* label, const MidMeshFaceAbResult& result);

// Gated self-test (logged AB_SELFTEST): feeds the harness a deliberately
// DUPLICATED gpu vector and a MISSING-face gpu vector and asserts it reports the
// duplicate as a multiplicityMismatch/extra and the missing as missingCpuFaces.
// Proves the comparison is a true multiset, not a set that silently passes
// duplicates. Returns true if every self-test assertion held.
bool RunMidMeshFaceAbSelfTest();

} // namespace VENPOD::Graphics
