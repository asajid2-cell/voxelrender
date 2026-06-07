param(
    [string]$NormalFrame,
    [string]$LogPath,
    [int]$Frame,
    [string]$OwnerFrame = "",
    [string]$MaterialFrame = "",
    [string]$LodFrame = "",
    [string]$FogFrame = "",
    [string]$WaterFrame = "",
    [string]$ClosureFrame = "",
    [string]$OutputCsv,
    [int]$MaxRows = 64
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($NormalFrame)) { throw "-NormalFrame is required" }
if ([string]::IsNullOrWhiteSpace($LogPath)) { throw "-LogPath is required" }
if ([string]::IsNullOrWhiteSpace($OutputCsv)) { throw "-OutputCsv is required" }

$source = @"
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Globalization;
using System.IO;
using System.Text;
using System.Text.RegularExpressions;

public static class VisualBadPixelAudit
{
    const float SeaLevel = -48.0f;
    const uint Seed = 12345u;
    const float MidStart = 1024.0f;
    const float MidEnd = 6400.0f;
    const int MidRings = 4;

    struct V3 {
        public double X, Y, Z;
        public V3(double x, double y, double z) { X = x; Y = y; Z = z; }
        public static V3 operator +(V3 a, V3 b) { return new V3(a.X + b.X, a.Y + b.Y, a.Z + b.Z); }
        public static V3 operator *(V3 a, double s) { return new V3(a.X * s, a.Y * s, a.Z * s); }
    }
    struct Camera { public V3 Pos, Forward; public double Yaw, Pitch; }
    struct Hit { public bool Found; public float T, Height; public V3 Pos; public string Material; }

    public static void Run(string normalPath, string logPath, int frame, string ownerPath, string materialPath, string lodPath, string fogPath, string waterPath, string closurePath, string outputCsv, int maxRows)
    {
        Camera camera = ParseCamera(logPath, frame);
        Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(outputCsv)));
        using (Bitmap normal = new Bitmap(normalPath))
        using (Bitmap owner = LoadOptional(ownerPath))
        using (Bitmap material = LoadOptional(materialPath))
        using (Bitmap lod = LoadOptional(lodPath))
        using (Bitmap fog = LoadOptional(fogPath))
        using (Bitmap water = LoadOptional(waterPath))
        using (Bitmap closure = LoadOptional(closurePath))
        using (StreamWriter writer = new StreamWriter(outputCsv, false, Encoding.UTF8))
        {
            List<Tuple<string,int,int,Color>> pixels = PickBadPixels(normal, maxRows);
            writer.WriteLine("frame,region,pixelX,pixelY,normalR,normalG,normalB,ownerLayer,materialDebug,estimatedDistance,worldX,worldY,worldZ,midRing,midCellSize,lodDebug,farSvoState,closureContribution,fogAmount,waterState,waterCandidateDistance,waterRejectionReason,cpuTerrainMaterialAtHit,shaderTerrainMaterialAtHit,geometryState,likelyCause");
            foreach (Tuple<string,int,int,Color> pixel in pixels)
            {
                V3 ray = BuildRay(camera, normal.Width, normal.Height, pixel.Item2, pixel.Item3);
                Hit hit = FindTerrain(camera.Pos, ray, 32.0f, 10400.0f, 4.0f);
                string ownerLayer = ClassifyOwner(Sample(owner, pixel.Item2, pixel.Item3));
                string materialDebug = ClassifyMaterial(Sample(material, pixel.Item2, pixel.Item3));
                string lodDebug = ClassifyLod(Sample(lod, pixel.Item2, pixel.Item3), ownerLayer, hit.Found ? hit.T : 0.0f);
                string closureContribution = ClassifyClosure(Sample(closure, pixel.Item2, pixel.Item3));
                double fogAmount = SampleLuma(fog, pixel.Item2, pixel.Item3);
                string waterState = ClassifyWater(Sample(water, pixel.Item2, pixel.Item3), ownerLayer);
                float waterT = WaterCandidateT(camera.Pos, ray);
                string waterReject = WaterRejectReason(camera.Pos, ray, waterT, hit);
                int ring = hit.Found ? PreferredRing(hit.T) : -1;
                string cellSize = ring >= 0 ? F(MidCellSize(ring)) : "";
                string geom = hit.Found ? "cpu_shader_expected_solid" : "cpu_shader_air";
                string likely = LikelyCause(ownerLayer, materialDebug, closureContribution, waterState, hit.Found);
                writer.WriteLine(string.Join(",",
                    frame.ToString(CultureInfo.InvariantCulture),
                    Csv(pixel.Item1),
                    pixel.Item2.ToString(CultureInfo.InvariantCulture),
                    pixel.Item3.ToString(CultureInfo.InvariantCulture),
                    pixel.Item4.R.ToString(CultureInfo.InvariantCulture),
                    pixel.Item4.G.ToString(CultureInfo.InvariantCulture),
                    pixel.Item4.B.ToString(CultureInfo.InvariantCulture),
                    Csv(ownerLayer),
                    Csv(materialDebug),
                    hit.Found ? F(hit.T) : "",
                    hit.Found ? F(hit.Pos.X) : "",
                    hit.Found ? F(hit.Pos.Y) : "",
                    hit.Found ? F(hit.Pos.Z) : "",
                    ring >= 0 ? ring.ToString(CultureInfo.InvariantCulture) : "",
                    cellSize,
                    Csv(lodDebug),
                    Csv("runtime_complete_from_capture_log"),
                    Csv(closureContribution),
                    F(fogAmount),
                    Csv(waterState),
                    waterT > 0.0f ? F(waterT) : "",
                    Csv(waterReject),
                    Csv(hit.Found ? hit.Material : "air"),
                    Csv(hit.Found ? hit.Material : "air"),
                    Csv(geom),
                    Csv(likely)));
            }
        }
    }

    static List<Tuple<string,int,int,Color>> PickBadPixels(Bitmap bitmap, int maxRows)
    {
        List<Tuple<string,int,int,Color>> result = new List<Tuple<string,int,int,Color>>();
        Dictionary<string,int> quota = new Dictionary<string,int> {
            {"upper_grey_mass", Math.Max(8, maxRows / 3)},
            {"mid_fuzzy_floating", Math.Max(8, maxRows / 3)},
            {"shoreline_or_foreground", Math.Max(8, maxRows - (maxRows / 3) * 2)}
        };
        Dictionary<string,int> counts = new Dictionary<string,int> {
            {"upper_grey_mass", 0},
            {"mid_fuzzy_floating", 0},
            {"shoreline_or_foreground", 0}
        };
        int step = Math.Max(10, Math.Min(bitmap.Width, bitmap.Height) / 40);
        for (int y = Math.Max(80, bitmap.Height / 8); y < bitmap.Height - Math.Max(80, bitmap.Height / 8); y += step) {
            for (int x = bitmap.Width / 12; x < bitmap.Width - bitmap.Width / 18; x += step) {
                Color c = bitmap.GetPixel(x, y);
                int max = Math.Max(c.R, Math.Max(c.G, c.B));
                int min = Math.Min(c.R, Math.Min(c.G, c.B));
                bool grayFuzzy = max - min < 42 && c.R > 70 && c.R < 175 && c.G > 70 && c.G < 175 && c.B > 70 && c.B < 175;
                bool mutedChunk = c.R > 85 && c.G > 80 && c.B > 70 && c.R < 185 && c.G < 180 && c.B < 170 && c.G - c.B < 42;
                bool notOverlay = !(x < bitmap.Width * 0.30 && y < bitmap.Height * 0.28);
                if ((grayFuzzy || mutedChunk) && notOverlay) {
                    string region = y < bitmap.Height * 0.36 ? "upper_grey_mass" :
                        (y < bitmap.Height * 0.62 ? "mid_fuzzy_floating" : "shoreline_or_foreground");
                    if (counts[region] >= quota[region]) continue;
                    result.Add(Tuple.Create(region, x, y, c));
                    counts[region]++;
                    if (result.Count >= maxRows) return result;
                }
            }
        }
        return result;
    }

    static Camera ParseCamera(string logPath, int frame)
    {
        string text = File.ReadAllText(logPath);
        Regex re = new Regex(@"PERF_(?:SPARSE_WALK|CAMERA_EXPOSURE) frame=" + frame + @".*cam=\(([-0-9.]+),([-0-9.]+),([-0-9.]+)\).*yaw=([-0-9.]+) pitch=([-0-9.]+) forward=\(([-0-9.]+),([-0-9.]+),([-0-9.]+)\)");
        Match m = re.Match(text);
        if (!m.Success) throw new InvalidOperationException("Could not find PERF_SPARSE_WALK or PERF_CAMERA_EXPOSURE frame=" + frame);
        return new Camera {
            Pos = new V3(D(m, 1), D(m, 2), D(m, 3)),
            Yaw = D(m, 4),
            Pitch = D(m, 5),
            Forward = Normalize(new V3(D(m, 6), D(m, 7), D(m, 8)))
        };
    }

    static V3 BuildRay(Camera c, int width, int height, int px, int py)
    {
        double fov = 60.0 * Math.PI / 180.0;
        double aspect = (double)width / Math.Max(1, height);
        double ndcX = (((px + 0.5) / width) * 2.0 - 1.0);
        double ndcY = -(((py + 0.5) / height) * 2.0 - 1.0);
        V3 forward = c.Forward;
        V3 worldUp = new V3(0, 1, 0);
        V3 right = Normalize(Cross(forward, worldUp));
        V3 up = Normalize(Cross(right, forward));
        double tanHalf = Math.Tan(fov * 0.5);
        return Normalize(new V3(
            forward.X + right.X * ndcX * tanHalf * aspect + up.X * ndcY * tanHalf,
            forward.Y + right.Y * ndcX * tanHalf * aspect + up.Y * ndcY * tanHalf,
            forward.Z + right.Z * ndcX * tanHalf * aspect + up.Z * ndcY * tanHalf));
    }

    static Hit FindTerrain(V3 origin, V3 ray, float start, float end, float step)
    {
        for (float t = start; t <= end; t += step) {
            V3 p = origin + ray * t;
            float h = HeightAt((float)p.X, (float)p.Z);
            float surface = h < SeaLevel ? SeaLevel : h;
            if (p.Y <= surface) {
                return new Hit { Found = true, T = t, Pos = p, Height = h, Material = MaterialAt(h, p.Y) };
            }
        }
        return new Hit { Found = false };
    }

    static float HeightAt(float x, float z)
    {
        float broad = ValueNoise2D(x * 0.0045f, z * 0.0045f, Seed + 11u);
        float ridgeSource = ValueNoise2D(x * 0.0100f + 41.0f, z * 0.0100f - 17.0f, Seed + 23u);
        float ridge = 1.0f - Math.Abs(ridgeSource);
        float detail = ValueNoise2D(x * 0.035f - 13.0f, z * 0.035f + 29.0f, Seed + 37u);
        float ridgeHeight = ridge * ridge;
        float height = -64.0f + broad * 145.0f + ridgeHeight * 150.0f + detail * 8.0f;
        float dx = x - 192.0f, dz = z - 224.0f;
        float originDistance = (float)Math.Sqrt(dx * dx + dz * dz);
        float originComfort = 1.0f - Smooth(Clamp((originDistance - 180.0f) / 520.0f));
        float publicRegionHeight = -42.0f + broad * 54.0f + ridgeHeight * 48.0f + detail * 3.0f + (1.0f - Smooth(originDistance / 360.0f)) * 72.0f;
        height += (1.0f - Smooth(originDistance / 420.0f)) * 58.0f;
        height = Lerp(height, publicRegionHeight, originComfort * 0.94f);
        float publicCapInfluence = 1.0f - Smooth(Clamp((originDistance - 220.0f) / 420.0f));
        float publicCap = 58.0f + Smooth(Clamp(originDistance / 640.0f)) * 114.0f;
        height = Lerp(height, Math.Min(height, publicCap), publicCapInfluence);
        float submergedBlend = 1.0f - Smooth(Clamp((height - (SeaLevel + 28.0f)) / 86.0f));
        if (submergedBlend > 0.0f) {
            float shelf = (SeaLevel - 8.0f) + broad * 38.0f + ridgeHeight * 22.0f + detail * 2.0f + (1.0f - Smooth(originDistance / 520.0f)) * 18.0f;
            height = Lerp(height, shelf, submergedBlend * 0.55f);
        }
        float playableBankBand = 1.0f - Smooth(Clamp((originDistance - 260.0f) / 980.0f));
        float lowlandUpper = 1.0f - Smooth(Clamp((height - (SeaLevel + 96.0f)) / 120.0f));
        float lowlandFloor = Smooth(Clamp((height - (SeaLevel - 40.0f)) / 64.0f));
        float playableBankBlend = playableBankBand * lowlandUpper * lowlandFloor * 0.64f;
        if (playableBankBlend > 0.0f) {
            float playableShelfHeight = (SeaLevel + 18.0f) + broad * 28.0f + ridgeHeight * 10.0f + detail * 1.5f + (1.0f - Smooth(Clamp(originDistance / 460.0f))) * 42.0f;
            height = Lerp(height, playableShelfHeight, playableBankBlend);
        }
        float publicBasinBand =
            Smooth(Clamp((originDistance - 360.0f) / 240.0f)) *
            (1.0f - Smooth(Clamp((originDistance - 1700.0f) / 760.0f))) *
            Smooth(Clamp((height - (SeaLevel - 38.0f)) / 56.0f)) *
            (1.0f - Smooth(Clamp((height - (SeaLevel + 180.0f)) / 140.0f)));
        if (publicBasinBand > 0.0f) {
            float publicBasinFloor = (SeaLevel - 12.0f) + broad * 2.0f + detail * 0.35f;
            height = Lerp(height, Math.Min(height, publicBasinFloor), publicBasinBand * 0.80f);
        }
        float backdropNoise = ValueNoise2D(x * 0.0018f + 19.0f, z * 0.0018f - 31.0f, Seed + 211u);
        float backdropRidgeSource = ValueNoise2D(x * 0.0032f - 71.0f, z * 0.0032f + 43.0f, Seed + 227u);
        float backdropRidge = 1.0f - Math.Abs(backdropRidgeSource);
        float backdropBreakup = ValueNoise2D(x * 0.0075f + 203.0f, z * 0.0075f - 167.0f, Seed + 271u);
        float backdropNotch = Smooth(Clamp((backdropBreakup - 0.08f) / 0.58f));
        float silhouetteRidge = Clamp(backdropRidge * backdropRidge * 1.22f + backdropNoise * 0.18f);
        float backdropBand = Smooth(Clamp((originDistance - 1360.0f) / 700.0f)) * (1.0f - Smooth(Clamp((originDistance - 5200.0f) / 1200.0f)));
        float northBackdrop = Smooth(Clamp((z - 1180.0f) / 900.0f));
        float sideBackdrop = Smooth(Clamp((Math.Abs(x - 192.0f) - 820.0f) / 980.0f));
        float backdropInfluence = backdropBand * Clamp(northBackdrop + sideBackdrop * 0.58f) * Smooth(silhouetteRidge) * (0.46f + backdropNotch * 0.54f);
        float backdropHeight = 248.0f + backdropBand * 160.0f + silhouetteRidge * 186.0f + backdropNoise * 26.0f;
        height = Lerp(height, Math.Max(height, backdropHeight), backdropInfluence * 0.70f);
        float westCorridor = Smooth(Clamp((192.0f - x - 520.0f) / 820.0f));
        float eastCorridor = Smooth(Clamp((x - 192.0f - 520.0f) / 820.0f));
        float southBlend = Smooth(Clamp((360.0f - z) / 1200.0f));
        float westNorthBlend = Smooth(Clamp((z - 360.0f) / 920.0f));
        float routeDistanceBand = Smooth(Clamp((originDistance - 780.0f) / 420.0f)) * (1.0f - Smooth(Clamp((originDistance - 4300.0f) / 1200.0f)));
        float routeCorridor = routeDistanceBand * Clamp(westCorridor * (0.50f + southBlend * 0.42f + westNorthBlend * 0.30f) + eastCorridor * southBlend);
        float routeRidgeNoiseA = ValueNoise2D(x * 0.0024f + 113.0f, z * 0.0024f - 89.0f, Seed + 251u);
        float routeRidgeNoiseB = ValueNoise2D(x * 0.0068f - 37.0f, z * 0.0068f + 151.0f, Seed + 263u);
        float routeBreakup = ValueNoise2D(x * 0.0110f - 211.0f, z * 0.0110f + 73.0f, Seed + 281u);
        float routeNotch = Smooth(Clamp((routeBreakup - 0.02f) / 0.60f));
        float routeRidge = Clamp(0.26f + (1.0f - Math.Abs(routeRidgeNoiseA)) * 0.58f + routeRidgeNoiseB * 0.16f);
        float routeBackdropHeight = 272.0f + routeDistanceBand * 104.0f + routeRidge * 218.0f;
        height = Lerp(height, Math.Max(height, routeBackdropHeight), routeCorridor * routeRidge * routeNotch * 0.68f);
        return Math.Max(-332.0f, Math.Min(664.0f, height));
    }

    static string MaterialAt(float height, double y) {
        if (y <= SeaLevel && height < SeaLevel) return "water";
        if (height < SeaLevel + 6.0f) return "sand";
        if (height > 160.0f) return "stone";
        return "dirt";
    }

    static string ClassifyOwner(Color c) {
        if (c.A == 0) return "unavailable";
        if (Near(c, 255,242,13,60)) return "exact_sparse_surface";
        if (Near(c, 13,242,64,70)) return "mid_voxel";
        if (Near(c, 51,107,255,70)) return "far_svo";
        if (Near(c, 5,199,255,70)) return "water";
        if (Near(c, 46,107,242,70)) return "sky";
        if (Near(c, 255,13,5,70)) return "miss";
        return "mixed_or_surface_overlay";
    }
    static string ClassifyMaterial(Color c) {
        if (c.A == 0) return "unavailable";
        if (Near(c, 13,97,255,40)) return "water";
        if (Near(c, 255,214,31,45)) return "sand";
        if (Near(c, 46,199,51,45)) return "dirt";
        if (Near(c, 140,140,140,45)) return "stone";
        if (Near(c, 8,10,15,35)) return "air";
        return "mixed";
    }
    static string ClassifyLod(Color c, string owner, float t) {
        if (c.A == 0) return "unavailable";
        if (owner == "mid_voxel") return "mid_color_ring_cell";
        if (owner == "far_svo") return "far_svo_cell_color";
        if (owner == "exact_sparse_surface") return "near_exact_band";
        return "not_voxel_lod_owner";
    }
    static string ClassifyClosure(Color c) {
        if (c.A == 0) return "unavailable";
        if (c.R > 180 && c.G < 80) return "route_closure_strong";
        if (c.R > 170 && c.G > 120) return "backdrop_closure_strong";
        if (c.G > c.R && c.G > c.B) return "base_terrain";
        return "mixed_closure";
    }
    static string ClassifyWater(Color c, string owner) {
        if (c.A == 0) return "unavailable";
        if (c.B > 180 && c.G > 120) return "water_owned_visible";
        if (c.R > 150 && c.G < 90) return "water_candidate_behind_terrain";
        if (owner == "water") return "water_owned_visible";
        return "no_visible_water_owner";
    }
    static string WaterRejectReason(V3 origin, V3 ray, float waterT, Hit hit) {
        if (waterT <= 0.0f) return "ray_not_down_to_water_plane";
        if (hit.Found && hit.T <= waterT + 0.25f) return "occluded_by_terrain_before_water";
        V3 p = origin + ray * waterT;
        float h = HeightAt((float)p.X, (float)p.Z);
        if (h >= SeaLevel) return "terrain_above_sea_at_water_candidate";
        return "water_candidate_available";
    }
    static float WaterCandidateT(V3 origin, V3 ray) {
        if (ray.Y >= -0.015 || ray.Y < -0.92) return -1.0f;
        float t = (float)((SeaLevel - origin.Y) / ray.Y);
        return t >= 32.0f && t <= 10400.0f ? t : -1.0f;
    }
    static string LikelyCause(string owner, string material, string closure, string water, bool solid) {
        if (closure.Contains("closure_strong")) return "procedural_closure_geometry";
        if (owner == "mid_voxel") return "mid_voxel_representation_or_normals";
        if (owner == "far_svo") return "far_svo_representation_or_normals";
        if (water.Contains("occluded") || water.Contains("behind")) return "terrain_owns_before_water";
        if (!solid) return "procedural_air_or_selection_miss";
        if (material == "stone" || material == "dirt") return "geometry_exists_material_or_fog_readability";
        return "mixed";
    }

    static int PreferredRing(float t) { return Math.Max(0, Math.Min(MidRings - 1, (int)Math.Floor(Clamp((t - MidStart) / Math.Max(MidEnd - MidStart, 1.0f)) * MidRings))); }
    static float MidCellSize(int ring) { return 12.0f * (float)Math.Pow(2.0, Math.Max(0, ring)); }
    static Bitmap LoadOptional(string path) { return String.IsNullOrWhiteSpace(path) || !File.Exists(path) ? null : new Bitmap(path); }
    static Color Sample(Bitmap b, int x, int y) { return b == null ? Color.FromArgb(0,0,0,0) : b.GetPixel(Math.Max(0, Math.Min(b.Width - 1, x)), Math.Max(0, Math.Min(b.Height - 1, y))); }
    static double SampleLuma(Bitmap b, int x, int y) { Color c = Sample(b, x, y); return c.A == 0 ? 0.0 : (0.2126 * c.R + 0.7152 * c.G + 0.0722 * c.B) / 255.0; }
    static bool Near(Color c, int r, int g, int b, int tol) { return Math.Abs(c.R-r)+Math.Abs(c.G-g)+Math.Abs(c.B-b) <= tol*3; }
    static double D(Match m, int group) { return double.Parse(m.Groups[group].Value, CultureInfo.InvariantCulture); }
    static string F(double v) { return v.ToString("0.###", CultureInfo.InvariantCulture); }
    static string Csv(string s) { return "\"" + (s ?? "").Replace("\"", "\"\"") + "\""; }
    static V3 Cross(V3 a, V3 b) { return new V3(a.Y*b.Z-a.Z*b.Y, a.Z*b.X-a.X*b.Z, a.X*b.Y-a.Y*b.X); }
    static V3 Normalize(V3 v) { double l = Math.Sqrt(v.X*v.X+v.Y*v.Y+v.Z*v.Z); return l <= 1e-9 ? new V3(0,0,1) : new V3(v.X/l,v.Y/l,v.Z/l); }
    static float Clamp(float v) { return Math.Max(0.0f, Math.Min(1.0f, v)); }
    static float Smooth(float v) { v = Clamp(v); return v * v * (3.0f - 2.0f * v); }
    static float Lerp(float a, float b, float t) { return a + (b - a) * Clamp(t); }
    static uint Hash2D(int x, int y, uint seed) { uint h = seed ^ 2166136261u; h = (h ^ (uint)x) * 16777619u; h = (h ^ (uint)y) * 16777619u; h ^= h >> 13; h *= 1274126177u; h ^= h >> 16; return h; }
    static float ValueNoise2D(float x, float y, uint seed) {
        int ix = (int)Math.Floor(x), iy = (int)Math.Floor(y);
        float fx = x - ix, fy = y - iy;
        float a = (Hash2D(ix, iy, seed) & 0xFFFFu) / 65535.0f;
        float b = (Hash2D(ix + 1, iy, seed) & 0xFFFFu) / 65535.0f;
        float c = (Hash2D(ix, iy + 1, seed) & 0xFFFFu) / 65535.0f;
        float d = (Hash2D(ix + 1, iy + 1, seed) & 0xFFFFu) / 65535.0f;
        float sx = Smooth(fx), sy = Smooth(fy);
        return Lerp(Lerp(a, b, sx), Lerp(c, d, sx), sy) * 2.0f - 1.0f;
    }
}
"@

Add-Type -TypeDefinition $source -ReferencedAssemblies System.Drawing
[VisualBadPixelAudit]::Run($NormalFrame, $LogPath, $Frame, $OwnerFrame, $MaterialFrame, $LodFrame, $FogFrame, $WaterFrame, $ClosureFrame, $OutputCsv, $MaxRows)
Write-Host "Wrote $OutputCsv"
