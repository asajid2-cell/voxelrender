 The source is mid-height raster mesh dirty publishing inside the near/mid-voxel overlap, not LOD merge and not the
  whole mid layer.

  Evidence from replay:

  frame 866: dirty=6, lodMerged=0, removed=0, miss=new=3 child=3
  frame 886: dirty=7, lodMerged=0, removed=0, miss=new=4 child=3
  frame 1276: dirty=8, lodMerged=0, removed=0, miss=new=5 child=3

  Then each of those frames immediately does:

  upload=dirty
  dirtyReject=accepted
  visibleMissing=0

  So the bug is not “mesh missing.” It is “new mid-height raster tiles become visible immediately.”

  The code path is:

  1. Z:/328/CMPUT328-A2/codexworks/301/3d/VENPOD/src/Simulation/SparseClipmap.cpp:8119 pre-extracts cache-miss mid-
     height tiles.

  2. Z:/328/CMPUT328-A2/codexworks/301/3d/VENPOD/src/Simulation/SparseClipmap.cpp:8330 marks those newly extracted tiles
     dirty:

  m_midMeshDirtyCoords.insert(coord);

  3. Z:/328/CMPUT328-A2/codexworks/301/3d/VENPOD/src/Simulation/SparseClipmap.cpp:8780 also marks any re-emitted/new
     tile dirty.

  4. Z:/328/CMPUT328-A2/codexworks/301/3d/VENPOD/src/Simulation/SparseClipmap.cpp:8843 exports them as
     midMeshSnapshot.dirtyBricks.

  5. Z:/328/CMPUT328-A2/codexworks/301/3d/VENPOD/src/main_launcher.cpp:17950 uploads them immediately through
  6. Z:/328/CMPUT328-A2/codexworks/301/3d/VENPOD/src/main_launcher.cpp:24377 draws near surface, then mid-height mesh,
     then raymarch.

  7. Z:/328/CMPUT328-A2/codexworks/301/3d/VENPOD/src/Graphics/Renderer.cpp:2115 the sparse surface pipeline writes
  So the visible wave is:

  mid-height tile becomes resident / child mask changes
  CPU extracts faces
  dirty upload accepts it
  raster mesh draws immediately
  stencil blocks the previous mid-voxel/raymarch result
  player sees an ownership/material/geometry swap

  The real fix should be around handoff policy, not disabling the layer. Specifically: keep rendering continuity, but
  stop newly uploaded mid-height tiles from instantly taking visible ownership in the overlap unless the replacement is
  ready/stable/blended.