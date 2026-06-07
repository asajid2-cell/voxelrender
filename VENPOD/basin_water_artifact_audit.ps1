param(
    [string]$NormalFrame,
    [string]$LogPath,
    [string]$OwnerFrame = "",
    [string]$MaterialFrame = "",
    [string]$FaceFrame = "",
    [int]$Frame = 500,
    [string]$OutputDir,
    [int]$MaxRows = 384
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($NormalFrame) -or !(Test-Path $NormalFrame)) {
    throw "Normal frame not found: $NormalFrame"
}
if ([string]::IsNullOrWhiteSpace($LogPath) -or !(Test-Path $LogPath)) {
    throw "Log not found: $LogPath"
}
if ([string]::IsNullOrWhiteSpace($OutputDir)) {
    throw "-OutputDir is required"
}

$source = @"
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.RegularExpressions;

public static class BasinWaterArtifactAudit
{
    const float SeaLevel = -48.0f;
    const float WaterSurfaceY = SeaLevel + 1.0f;
    const uint Seed = 12345u;

    struct V3 {
        public double X, Y, Z;
        public V3(double x, double y, double z) { X = x; Y = y; Z = z; }
        public static V3 operator +(V3 a, V3 b) { return new V3(a.X + b.X, a.Y + b.Y, a.Z + b.Z); }
        public static V3 operator *(V3 a, double s) { return new V3(a.X * s, a.Y * s, a.Z * s); }
    }

    struct Camera {
        public int Frame;
        public V3 Pos;
        public V3 Forward;
        public double Yaw, Pitch;
        public double SurfaceRasterMax;
    }

    struct TerrainHit {
        public bool Found;
        public float T;
        public V3 Pos;
        public float Height;
        public float Relief;
        public string Material;
    }

    struct PixelSample {
        public int X, Y;
        public Color Normal, Owner, Material, Face;
        public string Selection;
    }

    public static void Run(
        string normalPath,
        string logPath,
        string ownerPath,
        string materialPath,
        string facePath,
        int requestedFrame,
        string outputDir,
        int maxRows)
    {
        Directory.CreateDirectory(outputDir);
        Camera camera = ParseCamera(logPath, requestedFrame);

        using (Bitmap normal = new Bitmap(normalPath))
        using (Bitmap owner = LoadOptional(ownerPath))
        using (Bitmap material = LoadOptional(materialPath))
        using (Bitmap face = LoadOptional(facePath))
        {
            List<PixelSample> samples = PickBasinSamples(normal, owner, material, face, maxRows);
            string pixelsCsv = Path.Combine(outputDir, "basin_water_artifact_pixels.csv");
            using (StreamWriter writer = new StreamWriter(pixelsCsv, false, Encoding.UTF8)) {
                writer.WriteLine("requestedFrame,cameraFrame,pixelX,pixelY,selection,normalR,normalG,normalB,owner,materialDebug,faceDebug,rayTerrainT,worldX,worldY,worldZ,cpuTerrainHeight,cpuRelief,cpuMaterialAtRayHit,waterPlaneT,waterWorldX,waterWorldZ,terrainHeightAtWaterPlane,waterExpectedAtPlane,waterBeforeTerrain,reasonBucket,likelyCause");
                foreach (PixelSample s in samples) {
                    V3 ray = BuildRay(camera, normal.Width, normal.Height, s.X, s.Y);
                    TerrainHit terrainHit = FindFirstSurface(camera.Pos, ray, 8.0f, 6400.0f, 2.0f);
                    float waterT = WaterPlaneT(camera.Pos, ray);
                    V3 waterPos = waterT > 0.0f ? camera.Pos + ray * waterT : new V3(0, 0, 0);
                    float waterHeight = waterT > 0.0f ? HeightAtFloat((float)waterPos.X, (float)waterPos.Z) : -99999.0f;
                    bool waterExpected = waterT > 0.0f && waterHeight < SeaLevel;
                    bool waterBeforeTerrain = waterExpected && (!terrainHit.Found || waterT < terrainHit.T - 0.5f);
                    string ownerName = ClassifyOwner(s.Owner);
                    string materialName = ClassifyMaterial(s.Material);
                    string faceName = ClassifyFace(s.Face);
                    string reason = ReasonBucket(ownerName, materialName, terrainHit, waterExpected, waterBeforeTerrain);
                    string likely = LikelyCause(reason, ownerName, materialName, faceName, terrainHit, waterHeight);

                    writer.WriteLine(String.Join(",",
                        requestedFrame.ToString(CultureInfo.InvariantCulture),
                        camera.Frame.ToString(CultureInfo.InvariantCulture),
                        s.X.ToString(CultureInfo.InvariantCulture),
                        s.Y.ToString(CultureInfo.InvariantCulture),
                        Csv(s.Selection),
                        s.Normal.R.ToString(CultureInfo.InvariantCulture),
                        s.Normal.G.ToString(CultureInfo.InvariantCulture),
                        s.Normal.B.ToString(CultureInfo.InvariantCulture),
                        Csv(ownerName),
                        Csv(materialName),
                        Csv(faceName),
                        terrainHit.Found ? F(terrainHit.T) : "",
                        terrainHit.Found ? F(terrainHit.Pos.X) : "",
                        terrainHit.Found ? F(terrainHit.Pos.Y) : "",
                        terrainHit.Found ? F(terrainHit.Pos.Z) : "",
                        terrainHit.Found ? F(terrainHit.Height) : "",
                        terrainHit.Found ? F(terrainHit.Relief) : "",
                        Csv(terrainHit.Found ? terrainHit.Material : "air"),
                        waterT > 0.0f ? F(waterT) : "",
                        waterT > 0.0f ? F(waterPos.X) : "",
                        waterT > 0.0f ? F(waterPos.Z) : "",
                        waterT > 0.0f ? F(waterHeight) : "",
                        waterExpected ? "1" : "0",
                        waterBeforeTerrain ? "1" : "0",
                        Csv(reason),
                        Csv(likely)));
                }
            }

            string summaryCsv = Path.Combine(outputDir, "basin_water_artifact_summary.csv");
            using (StreamWriter writer = new StreamWriter(summaryCsv, false, Encoding.UTF8)) {
                writer.WriteLine("metric,value");
                writer.WriteLine("requestedFrame," + requestedFrame.ToString(CultureInfo.InvariantCulture));
                writer.WriteLine("cameraFrame," + camera.Frame.ToString(CultureInfo.InvariantCulture));
                writer.WriteLine("sampleCount," + samples.Count.ToString(CultureInfo.InvariantCulture));

                var rows = File.ReadAllLines(pixelsCsv).Skip(1)
                    .Select(line => SplitCsv(line))
                    .Where(cols => cols.Length >= 26)
                    .ToList();
                foreach (var g in rows.GroupBy(cols => cols[25]).OrderByDescending(g => g.Count())) {
                    writer.WriteLine(Csv("reason:" + g.Key) + "," + g.Count().ToString(CultureInfo.InvariantCulture));
                }
                foreach (var g in rows.GroupBy(cols => cols[8] + "|" + cols[9] + "|" + cols[10]).OrderByDescending(g => g.Count()).Take(16)) {
                    writer.WriteLine(Csv("owner_material_face:" + g.Key) + "," + g.Count().ToString(CultureInfo.InvariantCulture));
                }
                foreach (var g in rows.GroupBy(cols => cols[4]).OrderByDescending(g => g.Count())) {
                    writer.WriteLine(Csv("selection:" + g.Key) + "," + g.Count().ToString(CultureInfo.InvariantCulture));
                }
            }

            string overlayPath = Path.Combine(outputDir, "basin_water_artifact_overlay.bmp");
            using (Bitmap overlay = new Bitmap(normal))
            using (Graphics graphics = Graphics.FromImage(overlay)) {
                foreach (PixelSample s in samples) {
                    Color color = s.Selection == "blue_water" ? Color.Cyan :
                        (s.Selection == "gray_basin_patch" ? Color.Magenta : Color.Yellow);
                    using (Pen p = new Pen(color, 2.0f)) {
                        graphics.DrawRectangle(p, s.X - 3, s.Y - 3, 6, 6);
                    }
                }
                overlay.Save(overlayPath);
            }
        }
    }

    static List<PixelSample> PickBasinSamples(Bitmap normal, Bitmap owner, Bitmap material, Bitmap face, int maxRows)
    {
        int w = normal.Width;
        int h = normal.Height;
        int y0 = (int)(h * 0.48);
        int y1 = (int)(h * 0.78);
        int x0 = (int)(w * 0.05);
        int x1 = (int)(w * 0.88);
        int step = Math.Max(4, Math.Min(w, h) / 160);
        var result = new List<PixelSample>();
        var quotas = new Dictionary<string, int> {
            { "gray_basin_patch", Math.Max(48, maxRows / 2) },
            { "terrain_near_water", Math.Max(32, maxRows / 3) },
            { "blue_water", Math.Max(32, maxRows / 5) }
        };
        var counts = quotas.Keys.ToDictionary(k => k, k => 0);

        bool[,] water = new bool[w, h];
        for (int y = y0; y < y1; ++y) {
            for (int x = x0; x < x1; ++x) {
                Color c = normal.GetPixel(x, y);
                water[x, y] = IsBlueWater(c);
            }
        }

        for (int y = y0; y < y1; y += step) {
            for (int x = x0; x < x1; x += step) {
                Color c = normal.GetPixel(x, y);
                string selection = null;
                if (IsBlueWater(c)) {
                    selection = "blue_water";
                } else if (IsGrayBasinPatch(c)) {
                    selection = "gray_basin_patch";
                } else if (IsTerrainLike(c) && NearWater(water, w, h, x, y, 18)) {
                    selection = "terrain_near_water";
                }
                if (selection == null || counts[selection] >= quotas[selection]) continue;
                result.Add(new PixelSample {
                    X = x,
                    Y = y,
                    Normal = c,
                    Owner = Sample(owner, x, y),
                    Material = Sample(material, x, y),
                    Face = Sample(face, x, y),
                    Selection = selection
                });
                counts[selection]++;
                if (result.Count >= maxRows) return result;
            }
        }
        return result;
    }

    static Camera ParseCamera(string logPath, int requestedFrame)
    {
        Regex re = new Regex(@"PERF_CAMERA_EXPOSURE frame=(\d+).*cam=\(([-0-9.]+),([-0-9.]+),([-0-9.]+)\).*yaw=([-0-9.]+) pitch=([-0-9.]+) forward=\(([-0-9.]+),([-0-9.]+),([-0-9.]+)\).*surfaceRasterMax=([-0-9.]+)");
        var cameras = new List<Camera>();
        foreach (string line in File.ReadLines(logPath)) {
            Match m = re.Match(line);
            if (!m.Success) continue;
            cameras.Add(new Camera {
                Frame = I(m, 1),
                Pos = new V3(D(m, 2), D(m, 3), D(m, 4)),
                Yaw = D(m, 5),
                Pitch = D(m, 6),
                Forward = Normalize(new V3(D(m, 7), D(m, 8), D(m, 9))),
                SurfaceRasterMax = D(m, 10)
            });
        }
        if (cameras.Count == 0) throw new InvalidOperationException("No PERF_CAMERA_EXPOSURE rows found in " + logPath);
        Camera exact = cameras.FirstOrDefault(c => c.Frame == requestedFrame);
        if (exact.Frame != 0 || requestedFrame == 0) {
            return exact;
        }
        Camera nearest = cameras
            .OrderBy(c => Math.Abs(c.Frame - requestedFrame))
            .ThenByDescending(c => c.Frame <= requestedFrame ? 1 : 0)
            .First();
        throw new InvalidOperationException(
            "No exact PERF_CAMERA_EXPOSURE row for requested frame " + requestedFrame.ToString(CultureInfo.InvariantCulture) +
            " in " + logPath +
            " (nearest frame " + nearest.Frame.ToString(CultureInfo.InvariantCulture) + "). " +
            "Re-run capture with current engine so camera exposure is logged on capture frames.");
    }

    static V3 BuildRay(Camera c, int width, int height, int px, int py)
    {
        double fov = 60.0 * Math.PI / 180.0;
        double aspect = (double)width / Math.Max(1, height);
        double ndcX = (((px + 0.5) / width) * 2.0 - 1.0);
        double ndcY = -(((py + 0.5) / height) * 2.0 - 1.0);
        V3 forward = c.Forward;
        V3 upWorld = new V3(0, 1, 0);
        V3 right = Normalize(Cross(forward, upWorld));
        V3 up = Normalize(Cross(right, forward));
        double tanHalf = Math.Tan(fov * 0.5);
        return Normalize(new V3(
            forward.X + right.X * ndcX * tanHalf * aspect + up.X * ndcY * tanHalf,
            forward.Y + right.Y * ndcX * tanHalf * aspect + up.Y * ndcY * tanHalf,
            forward.Z + right.Z * ndcX * tanHalf * aspect + up.Z * ndcY * tanHalf));
    }

    static TerrainHit FindFirstSurface(V3 origin, V3 ray, float start, float end, float step)
    {
        for (float t = start; t <= end; t += step) {
            V3 p = origin + ray * t;
            int wx = RoundToInt(p.X);
            int wy = RoundToInt(p.Y);
            int wz = RoundToInt(p.Z);
            float height = HeightAt(wx, wz);
            float surface = height < SeaLevel ? WaterSurfaceY : height;
            if (p.Y <= surface) {
                float relief = SurfaceReliefAt(wx, wz, 4);
                return new TerrainHit {
                    Found = true,
                    T = t,
                    Pos = p,
                    Height = height,
                    Relief = relief,
                    Material = MaterialAt(height, relief, wy)
                };
            }
        }
        return new TerrainHit { Found = false };
    }

    static string MaterialAt(float height, float relief, int worldY)
    {
        if (worldY <= WaterSurfaceY && height < SeaLevel) return "water";
        float depth = height - worldY;
        bool nearWaterlineBank =
            height >= SeaLevel - 2.0f &&
            height < SeaLevel + 72.0f &&
            worldY <= SeaLevel + 14.0f &&
            depth < 96.0f;
        bool lowlandExposedBank =
            height >= SeaLevel + 18.0f &&
            height < SeaLevel + 128.0f &&
            worldY <= SeaLevel + 96.0f &&
            depth < 72.0f;
        bool dryOrIntertidalSurface =
            height >= SeaLevel - 2.0f &&
            height < SeaLevel + 6.0f;
        bool lowlandShoreTop = height < SeaLevel + 72.0f && depth < 4.0f;
        if (height < SeaLevel && depth < 6.0f) return "dirt";
        if (dryOrIntertidalSurface && depth < 16.0f && relief < 14.0f) return "sand";
        if (lowlandShoreTop) return (height < SeaLevel + 48.0f && relief < 36.0f) ? "sand" : "dirt";
        if (nearWaterlineBank) return (height < SeaLevel + 72.0f && relief < 52.0f && depth < 96.0f) ? "sand" : "dirt";
        if (lowlandExposedBank) return (height < SeaLevel + 86.0f && relief < 58.0f && depth < 42.0f) ? "sand" : "dirt";
        if (depth < 2.0f && !(relief > 10.0f || height > 160.0f)) return "dirt";
        if (depth < 5.0f && relief < 6.0f) return "dirt";
        return "stone";
    }

    static float HeightAt(int worldX, int worldZ)
    {
        return HeightAtFloat((float)worldX, (float)worldZ);
    }

    static float HeightAtFloat(float x, float z)
    {
        float broad = ValueNoise2D(x * 0.0045f, z * 0.0045f, Seed + 11u);
        float ridgeSource = ValueNoise2D(x * 0.0100f + 41.0f, z * 0.0100f - 17.0f, Seed + 23u);
        float ridge = 1.0f - Math.Abs(ridgeSource);
        float detail = ValueNoise2D(x * 0.035f - 13.0f, z * 0.035f + 29.0f, Seed + 37u);
        float ridgeHeight = ridge * ridge;

        float height = -64.0f + broad * 145.0f + ridgeHeight * 150.0f + detail * 8.0f;
        float originDx = x - 192.0f;
        float originDz = z - 224.0f;
        float originDistance = (float)Math.Sqrt(originDx * originDx + originDz * originDz);
        float originComfort = 1.0f - Smooth(Clamp((originDistance - 180.0f) / 520.0f));
        float publicRegionHeight =
            -42.0f + broad * 54.0f + ridgeHeight * 48.0f + detail * 3.0f +
            (1.0f - Smooth(originDistance / 360.0f)) * 72.0f;
        height += (1.0f - Smooth(originDistance / 420.0f)) * 58.0f;
        height = Lerp(height, publicRegionHeight, originComfort * 0.94f);

        float publicCapInfluence = 1.0f - Smooth(Clamp((originDistance - 220.0f) / 420.0f));
        float publicCap = 58.0f + Smooth(Clamp(originDistance / 640.0f)) * 114.0f;
        height = Lerp(height, Math.Min(height, publicCap), publicCapInfluence);

        float submergedBlend = 1.0f - Smooth(Clamp((height - (SeaLevel + 28.0f)) / 86.0f));
        if (submergedBlend > 0.0f) {
            float submergedShelfHeight =
                (SeaLevel - 8.0f) + broad * 38.0f + ridgeHeight * 22.0f + detail * 2.0f +
                (1.0f - Smooth(originDistance / 520.0f)) * 18.0f;
            height = Lerp(height, submergedShelfHeight, submergedBlend * 0.55f);
        }

        float playableBankBand = 1.0f - Smooth(Clamp((originDistance - 260.0f) / 980.0f));
        float lowlandUpper = 1.0f - Smooth(Clamp((height - (SeaLevel + 96.0f)) / 120.0f));
        float lowlandFloor = Smooth(Clamp((height - (SeaLevel - 40.0f)) / 64.0f));
        float playableBankBlend = playableBankBand * lowlandUpper * lowlandFloor * 0.64f;
        if (playableBankBlend > 0.0f) {
            float playableShelfHeight =
                (SeaLevel + 18.0f) + broad * 28.0f + ridgeHeight * 10.0f + detail * 1.5f +
                (1.0f - Smooth(Clamp(originDistance / 460.0f))) * 42.0f;
            height = Lerp(height, playableShelfHeight, playableBankBlend);
        }

        float publicBasinBand =
            Smooth(Clamp((originDistance - 360.0f) / 240.0f)) *
            (1.0f - Smooth(Clamp((originDistance - 1700.0f) / 760.0f))) *
            Smooth(Clamp((height - (SeaLevel - 38.0f)) / 56.0f)) *
            (1.0f - Smooth(Clamp((height - (SeaLevel + 180.0f)) / 140.0f)));
        float publicBasinFloor = (SeaLevel - 12.0f) + broad * 2.0f + detail * 0.35f;
        if (publicBasinBand > 0.0f) {
            height = Lerp(height, Math.Min(height, publicBasinFloor), publicBasinBand * 0.80f);
        }
        float backdropNoise = ValueNoise2D(x * 0.0018f + 19.0f, z * 0.0018f - 31.0f, Seed + 211u);
        float backdropRidgeSource = ValueNoise2D(x * 0.0032f - 71.0f, z * 0.0032f + 43.0f, Seed + 227u);
        float backdropRidge = 1.0f - Math.Abs(backdropRidgeSource);
        float backdropBreakup = ValueNoise2D(x * 0.0075f + 203.0f, z * 0.0075f - 167.0f, Seed + 271u);
        float backdropNotch = Smooth(Clamp((backdropBreakup - 0.08f) / 0.58f));
        float silhouetteRidge = Clamp(backdropRidge * backdropRidge * 1.22f + backdropNoise * 0.18f);
        float backdropBand =
            Smooth(Clamp((originDistance - 1360.0f) / 700.0f)) *
            (1.0f - Smooth(Clamp((originDistance - 5200.0f) / 1200.0f)));
        float northBackdrop = Smooth(Clamp((z - 1180.0f) / 900.0f));
        float sideBackdrop = Smooth(Clamp((Math.Abs(x - 192.0f) - 820.0f) / 980.0f));
        float backdropInfluence =
            backdropBand * Clamp(northBackdrop + sideBackdrop * 0.58f) *
            Smooth(silhouetteRidge) * (0.46f + backdropNotch * 0.54f);
        float backdropHeight = 248.0f + backdropBand * 160.0f + silhouetteRidge * 186.0f + backdropNoise * 26.0f;
        height = Lerp(height, Math.Max(height, backdropHeight), backdropInfluence * 0.70f);

        float westCorridor = Smooth(Clamp((192.0f - x - 520.0f) / 820.0f));
        float eastCorridor = Smooth(Clamp((x - 192.0f - 520.0f) / 820.0f));
        float southBlend = Smooth(Clamp((360.0f - z) / 1200.0f));
        float westNorthBlend = Smooth(Clamp((z - 360.0f) / 920.0f));
        float routeDistanceBand =
            Smooth(Clamp((originDistance - 780.0f) / 420.0f)) *
            (1.0f - Smooth(Clamp((originDistance - 4300.0f) / 1200.0f)));
        float routeCorridor = routeDistanceBand * Clamp(
            westCorridor * (0.50f + southBlend * 0.42f + westNorthBlend * 0.30f) +
            eastCorridor * southBlend);
        float routeRidgeNoiseA = ValueNoise2D(x * 0.0024f + 113.0f, z * 0.0024f - 89.0f, Seed + 251u);
        float routeRidgeNoiseB = ValueNoise2D(x * 0.0068f - 37.0f, z * 0.0068f + 151.0f, Seed + 263u);
        float routeBreakup = ValueNoise2D(x * 0.0110f - 211.0f, z * 0.0110f + 73.0f, Seed + 281u);
        float routeNotch = Smooth(Clamp((routeBreakup - 0.02f) / 0.60f));
        float routeRidge = Clamp(0.26f + (1.0f - Math.Abs(routeRidgeNoiseA)) * 0.58f + routeRidgeNoiseB * 0.16f);
        float routeBackdropHeight = 272.0f + routeDistanceBand * 104.0f + routeRidge * 218.0f;
        height = Lerp(height, Math.Max(height, routeBackdropHeight), routeCorridor * routeRidge * routeNotch * 0.68f);
        return Math.Max(-332.0f, Math.Min(664.0f, height));
    }

    static float SurfaceReliefAt(int worldX, int worldZ, int sampleOffset)
    {
        float center = HeightAt(worldX, worldZ);
        float minH = center, maxH = center;
        int o = Math.Max(1, sampleOffset);
        float[] values = {
            HeightAt(worldX - o, worldZ),
            HeightAt(worldX + o, worldZ),
            HeightAt(worldX, worldZ - o),
            HeightAt(worldX, worldZ + o)
        };
        foreach (float h in values) { minH = Math.Min(minH, h); maxH = Math.Max(maxH, h); }
        return maxH - minH;
    }

    static uint Hash3D(int x, int y, int z, uint seed)
    {
        uint h = seed ^ 2166136261u;
        h = (h ^ unchecked((uint)x)) * 16777619u;
        h = (h ^ unchecked((uint)y)) * 16777619u;
        h = (h ^ unchecked((uint)z)) * 16777619u;
        h ^= h >> 16;
        h *= 0x7feb352du;
        h ^= h >> 15;
        h *= 0x846ca68bu;
        h ^= h >> 16;
        return h;
    }

    static float ValueNoise2D(float x, float z, uint seed)
    {
        int x0 = (int)Math.Floor(x);
        int z0 = (int)Math.Floor(z);
        float fx = x - x0;
        float fz = z - z0;
        float sx = Smooth(fx);
        float sz = Smooth(fz);
        Func<int, int, float> sample = (ix, iz) =>
            (Hash3D(ix, 0, iz, seed) & 0xFFFFFFu) / (float)0xFFFFFFu;
        float a = Lerp(sample(x0, z0), sample(x0 + 1, z0), sx);
        float b = Lerp(sample(x0, z0 + 1), sample(x0 + 1, z0 + 1), sx);
        return Lerp(a, b, sz) * 2.0f - 1.0f;
    }

    static string ReasonBucket(string owner, string material, TerrainHit terrainHit, bool waterExpected, bool waterBeforeTerrain)
    {
        if (owner == "water" || material == "water") return "visible_water";
        if (waterBeforeTerrain && owner != "water") return "water_should_draw_before_terrain";
        if (terrainHit.Found && terrainHit.Material != "water" && terrainHit.Height >= SeaLevel) return "generated_land_above_sea";
        if (terrainHit.Found && terrainHit.Material != "water" && waterExpected) return "water_occluded_by_generated_terrain";
        if (!terrainHit.Found && waterExpected) return "water_expected_but_no_surface_hit";
        if (owner == "mid_voxel" || owner == "far_svo") return "coarse_layer_basin_terrain";
        return "mixed_or_unclassified";
    }

    static string LikelyCause(string reason, string owner, string material, string face, TerrainHit hit, float waterHeight)
    {
        if (reason == "visible_water") return "water rendered at sampled pixel";
        if (reason == "water_should_draw_before_terrain") return "ray reaches submerged water plane before CPU terrain; inspect water ownership/rejection";
        if (reason == "generated_land_above_sea") return "gray/terrain patch is generated terrain above sea level, not missing water";
        if (reason == "water_occluded_by_generated_terrain") return "terrain surface occurs before expected water plane";
        if (owner == "exact_sparse_surface" && face.Contains("side")) return "exact generated bank/side face dominates basin";
        if (owner == "mid_voxel" || owner == "far_svo") return "coarse fallback owns basin terrain";
        return "mixed; inspect pixel row";
    }

    static bool IsBlueWater(Color c) { return c.B >= 115 && c.G >= 75 && c.R <= 105 && c.B - c.R >= 35 && c.B - c.G >= 0; }
    static bool IsGrayBasinPatch(Color c) {
        int max = Math.Max(c.R, Math.Max(c.G, c.B));
        int min = Math.Min(c.R, Math.Min(c.G, c.B));
        return max - min < 42 && c.R >= 60 && c.R <= 165 && c.G >= 60 && c.G <= 165 && c.B >= 55 && c.B <= 155;
    }
    static bool IsTerrainLike(Color c) {
        return (c.G >= 65 && c.R >= 45 && c.B <= 150) ||
            (Math.Abs(c.R - c.G) < 45 && Math.Abs(c.G - c.B) < 48 && c.R > 55 && c.R < 185);
    }
    static bool NearWater(bool[,] water, int w, int h, int x, int y, int radius) {
        int x0 = Math.Max(0, x - radius), x1 = Math.Min(w - 1, x + radius);
        int y0 = Math.Max(0, y - radius), y1 = Math.Min(h - 1, y + radius);
        for (int yy = y0; yy <= y1; ++yy) for (int xx = x0; xx <= x1; ++xx) if (water[xx, yy]) return true;
        return false;
    }
    static string ClassifyOwner(Color c) {
        if (c.A == 0) return "unavailable";
        // Mode 55 far-water is a dark blue that also falls inside the broader
        // far-SVO tolerance. Classify it first or water pixels look like terrain.
        if (Near(c, 10, 71, 242, 28)) return "water";
        if (Near(c, 255, 13, 230, 45)) return "exact_sparse_surface_far";
        if (Near(c, 255, 117, 13, 45)) return "exact_sparse_surface_extended";
        if (Near(c, 255, 242, 13, 70)) return "exact_sparse_surface";
        if (Near(c, 13, 242, 64, 70)) return "mid_voxel";
        if (Near(c, 51, 107, 255, 80)) return "far_svo";
        if (Near(c, 5, 199, 255, 80)) return "water";
        if (Near(c, 46, 107, 242, 80)) return "sky";
        if (Near(c, 255, 13, 5, 80)) return "miss";
        return "mixed_or_unclassified";
    }
    static string ClassifyMaterial(Color c) {
        if (c.A == 0) return "unavailable";
        if (Near(c, 13, 97, 255, 45)) return "water";
        if (Near(c, 255, 214, 31, 50)) return "sand";
        if (Near(c, 46, 199, 51, 55)) return "dirt";
        if (Near(c, 140, 140, 140, 55)) return "stone";
        if (Near(c, 8, 10, 15, 40)) return "air";
        return "mixed";
    }
    static string ClassifyFace(Color c) {
        if (c.A == 0) return "unavailable";
        if (Near(c, 13, 242, 46, 70)) return "top";
        if (Near(c, 242, 13, 242, 70)) return "bottom_or_underside";
        if (Near(c, 255, 122, 13, 80)) return "side";
        if (Near(c, 255, 219, 13, 80)) return "other_face";
        return "mixed";
    }
    static float WaterPlaneT(V3 origin, V3 ray) {
        if (ray.Y >= -0.003) return -1.0f;
        float t = (float)((WaterSurfaceY - origin.Y) / ray.Y);
        return t >= 0.0f && t <= 10400.0f ? t : -1.0f;
    }
    static Bitmap LoadOptional(string path) { return String.IsNullOrWhiteSpace(path) || !File.Exists(path) ? null : new Bitmap(path); }
    static Color Sample(Bitmap b, int x, int y) { return b == null ? Color.FromArgb(0,0,0,0) : b.GetPixel(Math.Max(0, Math.Min(b.Width - 1, x)), Math.Max(0, Math.Min(b.Height - 1, y))); }
    static bool Near(Color c, int r, int g, int b, int tol) { return Math.Abs(c.R-r)+Math.Abs(c.G-g)+Math.Abs(c.B-b) <= tol*3; }
    static int RoundToInt(double v) { return (int)Math.Round(v); }
    static int I(Match m, int g) { return int.Parse(m.Groups[g].Value, CultureInfo.InvariantCulture); }
    static double D(Match m, int g) { return double.Parse(m.Groups[g].Value, CultureInfo.InvariantCulture); }
    static string F(double v) { return v.ToString("0.###", CultureInfo.InvariantCulture); }
    static string Csv(string s) { return "\"" + (s ?? "").Replace("\"", "\"\"") + "\""; }
    static string[] SplitCsv(string line) {
        var result = new List<string>();
        var sb = new StringBuilder();
        bool quoted = false;
        for (int i = 0; i < line.Length; ++i) {
            char ch = line[i];
            if (quoted) {
                if (ch == '"' && i + 1 < line.Length && line[i + 1] == '"') { sb.Append('"'); ++i; }
                else if (ch == '"') quoted = false;
                else sb.Append(ch);
            } else if (ch == '"') quoted = true;
            else if (ch == ',') { result.Add(sb.ToString()); sb.Clear(); }
            else sb.Append(ch);
        }
        result.Add(sb.ToString());
        return result.ToArray();
    }
    static V3 Cross(V3 a, V3 b) { return new V3(a.Y*b.Z-a.Z*b.Y, a.Z*b.X-a.X*b.Z, a.X*b.Y-a.Y*b.X); }
    static V3 Normalize(V3 v) { double l = Math.Sqrt(v.X*v.X+v.Y*v.Y+v.Z*v.Z); return l <= 1e-9 ? new V3(0,0,1) : new V3(v.X/l,v.Y/l,v.Z/l); }
    static float Clamp(float v) { return Math.Max(0.0f, Math.Min(1.0f, v)); }
    static float Smooth(float v) { v = Clamp(v); return v * v * (3.0f - 2.0f * v); }
    static float Lerp(float a, float b, float t) { return a + (b - a) * Clamp(t); }
}
"@

Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition $source
[BasinWaterArtifactAudit]::Run($NormalFrame, $LogPath, $OwnerFrame, $MaterialFrame, $FaceFrame, $Frame, $OutputDir, $MaxRows)
Write-Host "Wrote basin water artifact audit to $OutputDir"
