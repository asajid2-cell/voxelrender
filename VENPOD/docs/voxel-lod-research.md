# Voxel mid-LOD quality within a fixed budget — SOTA research (2026-06-11)

Deep-research synthesis (101-agent verified workflow, 21/25 claims confirmed) on how to make
VENPOD's coarse mid-distance voxel terrain read as a mild step down from the crisp near layer
("720p vs 1080p") **within the 16,384-brick cap** — not "2px vs 8K".

## The gap
Visible mid (1–3k units) renders as jaggy, aliased 16–32u coarse blocks vs the crisp 1u near.
Brute-force finer voxels is impossible: fine-everywhere ≈ 710k bricks vs the 16,384 cap (confirmed
empirically — uniform-fine and gentler-LOD-growth both break coverage → water floods the gap).

## Verified SOTA levers (ranked for VENPOD)

1. **Screen-space-error LOD cell sizing** *(highest payoff, cap-friendly, incremental)*
   Size each cell to a target **pixel footprint** instead of fixed distance bins. GigaVoxels descends
   until a voxel projects to ≤1px; geometry clipmaps target ~5px; Aokana exposes one tunable
   `LODError = ChunkSize·StreamingFactor − ‖center−camera‖` (SSIM ~0.9 at StreamingFactor 2.0). Cell
   count is bounded by **screen pixels, not world volume**, so it reallocates the fixed budget toward
   the visible mid. Sources: GigaVoxels (Crassin I3D'09 / DigiPen thesis), GPU Gems 2 Ch.2, Aokana (I3D'25).

2. **Geomorph the LOD-ring transitions** *(cures the cliff, not the coarseness)*
   Transition region near each ring edge blends geometry **and** surface shading (normals) between
   levels via α 0→1, instead of snapping. 2D-heightfield evidence (GPU Gems 2, Hoppe geomclipmap);
   the 3D-voxel analog is Transvoxel transition cells / POP-buffer / SDF blend. Cures popping + boundary
   aliasing — does **not** by itself fix coarse-block jaggies.

3. **AA done right + CAS sharpen (never sharpen-alone)** *(the blur/jaggy layer)*
   CAS (AMD FidelityFX) is explicitly a **complement to TAA that restores temporal-blur detail — not an
   AA technique**. So the fix for the 0.5-scale blur is a full-res or TAA-stabilized mid pass **for the
   AA**, with CAS layered on to recover crispness. This confirms the earlier 5-tap **unsharp was the
   wrong tool** (it sharpened the stair-steps → worse); FXAA/TAA is the right base. No verified
   head-to-head AA benchmark for voxel raymarch survived — VENPOD must measure in-engine.

4. **SVDAG-class compression** *(big ceiling-raiser, NOT drop-in — deferred)*
   Sparse Voxel DAGs cut node count 1–3 orders of magnitude (945MB for 19B voxels at 128K³; 576×;
   SSVDAG ~2× more; palette attribute compression for colors). **Caveat (verified):** every headline
   number is **static, precomputed, binary-geometry, high-self-similarity** showcase scenes
   (EpicCitadel, CrySponza). VENPOD is infinite, procedural, **colored**, low-self-similarity — an
   existence proof, *not* a promise of the same ratios, and a major structural change. Defer.

5. **Aggressive demand-paged streaming** (~5% resident covers huge distance — Aokana). Cap-type
   mismatch: VENPOD's cap is a fixed HLSL brick constant, not a VRAM-byte budget. Class transfers, not 1:1.

## Refuted / unverified (do not rely on)
- GigaVoxels "aliasing-free with 1 ray/pixel via quadrilinear MIP blend" — **refuted 1-2**.
- "n/10 transition width, ~10 VS instructions" geomorph specifics — **refuted 1-2** (concept stands, tuning unverified).
- SSVDAG "<15% tracing overhead" — **refuted 0-3**.
- **GPU procedural mid-brick regeneration to dodge the cap** — *no surviving evidence*; remains an
  unproven inference (was our leading hypothesis). Measure before betting on it.

## VENPOD decision (implementation order)
1. ✅ Full-res mid pass + FXAA (AA base; unsharp removed).
2. **Screen-space-error ring placement** — the main geometric fix (cap-bounded finer visible cells).
3. Geomorph the ring transitions.
4. (later) TAA + CAS for temporal stability; SVDAG only if pushing fine detail much further.

## Key sources
- GigaVoxels — Crassin et al. I3D 2009; DigiPen thesis (paged terrain).
- GPU Gems 2 Ch.2 geometry clipmaps; Hoppe geometry-clipmaps paper.
- High-Resolution Sparse Voxel DAGs — Kämpe/Sintorn/Assarsson, ACM TOG 2013.
- SSVDAG — Villanueva et al. I3D 2016; attribute DAG — Dado/Kol/Eisemann CGF 2016.
- Aokana — I3D 2025 (DOI 10.1145/3728299).
- AMD FidelityFX CAS — gpuopen.com/fidelityfx-cas.
