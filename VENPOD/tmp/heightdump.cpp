// Standalone terrain-height profiler: calls the REAL SparseTerrainGenerator::HeightAt
// to see the actual terrain elevation distribution by distance from origin.
#include "Simulation/SparseTerrainGenerator.h"
#include "Simulation/TerrainConstants.h"
#include <cstdio>
#include <cmath>
#include <algorithm>

using namespace VENPOD::Simulation;

// Stubs for symbols referenced by GenerateBrick/ComputeOccupancyAndFlags (which we never call).
namespace VENPOD { namespace Simulation {
    bool TryWorldVoxelFromBrickLocal(int, unsigned char, int*) { return false; }
    unsigned short LocalVoxelIndex(LocalVoxelCoord) { return 0; }
}}

int main() {
    SparseTerrainGenerator gen(12345u);
    const float originX = 192.0f, originZ = 224.0f; // the "origin" the height fn flattens around

    // 1) Radial profiles in 8 directions from origin, 0..9000 units.
    printf("=== RADIAL HEIGHT PROFILES (height above SEA_LEVEL_Y=%d) ===\n", SEA_LEVEL_Y);
    const char* dirName[8] = {"+X","+Z","-X","-Z","+XZ","-XZ","+X-Z","-X+Z"};
    const float dx[8] = {1,0,-1,0,0.707f,-0.707f,0.707f,-0.707f};
    const float dz[8] = {0,1,0,-1,0.707f,-0.707f,-0.707f,0.707f};
    for (int d = 0; d < 8; ++d) {
        printf("dir %-4s: ", dirName[d]);
        for (int dist = 0; dist <= 9000; dist += 750) {
            int x = (int)std::lround(originX + dx[d]*dist);
            int z = (int)std::lround(originZ + dz[d]*dist);
            float h = gen.HeightAt(x, z);
            printf("%5d ", (int)std::lround(h));
        }
        printf("\n");
    }
    printf("(columns = dist 0,750,1500,...,9000; values = world Y; sea=%d)\n\n", SEA_LEVEL_Y);

    // 2) Elevation stats per distance band (sampling a grid).
    printf("=== ELEVATION STATS BY DISTANCE BAND (relief = max-min over band) ===\n");
    struct Band { float lo, hi; float mn=1e9f, mx=-1e9f; double sum=0; int n=0; int above100=0; };
    Band bands[7] = {{0,400},{400,800},{800,1400},{1400,2400},{2400,4200},{4200,6400},{6400,9000}};
    for (int gx = -9000; gx <= 9000; gx += 120) {
        for (int gz = -9000; gz <= 9000; gz += 120) {
            float ddx = gx, ddz = gz;
            float dist = std::sqrt(ddx*ddx + ddz*ddz);
            float h = gen.HeightAt((int)std::lround(originX+gx), (int)std::lround(originZ+gz));
            for (auto& b : bands) {
                if (dist >= b.lo && dist < b.hi) {
                    b.mn = std::min(b.mn,h); b.mx = std::max(b.mx,h);
                    b.sum += h; b.n++;
                    if (h > (float)SEA_LEVEL_Y + 100.0f) b.above100++;
                    break;
                }
            }
        }
    }
    for (auto& b : bands) {
        if (b.n==0) continue;
        printf("dist %4.0f-%4.0f: min=%5d max=%5d avg=%5d relief=%4d  %%above(sea+100)=%4.1f%%\n",
            b.lo,b.hi,(int)std::lround(b.mn),(int)std::lround(b.mx),
            (int)std::lround(b.sum/b.n),(int)std::lround(b.mx-b.mn),
            100.0*b.above100/b.n);
    }
    return 0;
}
