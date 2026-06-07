param(
    [string]$FramePath = "build\captures\diag_walk_terrain_sky_reason_v5_20260519\engine_frame_0220.bmp",
    [string]$LogPath = "build\captures\diag_walk_terrain_sky_reason_v5_20260519\venpod_runtime.log",
    [string]$OutputCsv = "build\captures\diag_walk_terrain_sky_reason_v5_20260519\hole_ray_audit.csv",
    [int]$Frame = 220,
    [int]$MaxRows = 32,
    [switch]$AutoSelect,
    [switch]$DebugMissSelect,
    [string]$SamplePoints = ""
)

$ErrorActionPreference = "Stop"

$source = @"
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Globalization;
using System.IO;
using System.Text;
using System.Text.RegularExpressions;

public static class TerrainHoleRayAudit
{
    const float SeaLevel = -48.0f;
    const float TerrainMinY = -332.0f;
    const float TerrainMaxY = 664.0f;
    const float MidStart = 1024.0f;
    const float MidEnd = 6400.0f;
    const float MidCell = 12.0f;
    const int MidRings = 4;
    const float ExactNear = 1024.0f;
    const float DefaultSurfaceOwnershipRadius = 2048.0f;
    const uint Seed = 12345u;

    struct V3
    {
        public double X;
        public double Y;
        public double Z;
        public V3(double x, double y, double z) { X = x; Y = y; Z = z; }
        public static V3 operator +(V3 a, V3 b) { return new V3(a.X + b.X, a.Y + b.Y, a.Z + b.Z); }
        public static V3 operator *(V3 a, double s) { return new V3(a.X * s, a.Y * s, a.Z * s); }
    }

    struct PixelRegion
    {
        public string Name;
        public int X0;
        public int Y0;
        public int X1;
        public int Y1;
        public PixelRegion(string name, int x0, int y0, int x1, int y1)
        {
            Name = name; X0 = x0; Y0 = y0; X1 = x1; Y1 = y1;
        }
    }

    struct Camera
    {
        public V3 Pos;
        public V3 Forward;
        public double Yaw;
        public double Pitch;
    }

    public static void Run(string framePath, string logPath, string outputCsv, int frame, int maxRows, bool autoSelect, bool debugMissSelect, string samplePoints)
    {
        Camera camera = ParseCamera(logPath, frame);
        using (Bitmap bitmap = new Bitmap(framePath))
        {
            List<Tuple<string, int, int, Color>> pixels = !String.IsNullOrWhiteSpace(samplePoints)
                ? PickExplicitPixels(bitmap, samplePoints, maxRows)
                : (debugMissSelect
                    ? PickDebugMissPixels(bitmap, maxRows)
                    : (autoSelect
                        ? PickCurrentHolePixels(bitmap, maxRows)
                        : PickHolePixels(bitmap, maxRows)));
            Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(outputCsv)));
            RuntimeState runtime = ParseRuntimeState(logPath, frame);
            using (StreamWriter writer = new StreamWriter(outputCsv, false, Encoding.UTF8))
            {
                writer.WriteLine("frame,region,pixelX,pixelY,colorR,colorG,colorB,cameraX,cameraY,cameraZ,rayDirX,rayDirY,rayDirZ,backgroundAllowedStart,terrainDiagnosticStart,shaderFirstTerrainT,shaderTerrainBeforeAllowedStart,shaderFirstTerrainX,shaderFirstTerrainY,shaderFirstTerrainZ,shaderHeightAtHit,cpuFirstSolidT,cpuTerrainBeforeAllowedStart,cpuFirstSolidX,cpuFirstSolidY,cpuFirstSolidZ,cpuHeightAtHit,cpuMaterialAtHit,cpuSolidMatchesShader,firstExpectedTerrainT,firstExpectedBeforeAllowedStart,midPreferredRing,midCellSize,midBrickX,midBrickY,midBrickZ,midRuntimeRingResident,midRuntimeRingMissing,midRuntimeRingCoveragePct,midGeneratedMaterialAtExpected,midGeneratedSolidAtExpected,midRepresentationBucket,farSvoRuntimeReady,farSvoPageX,farSvoPageZ,farSvoPageInRange,farSvoGeneratedSolidAtExpected,dominantBucket,midResidentStatus,midMaterialSampled,rejectionReason,notes");
                foreach (Tuple<string, int, int, Color> pixel in pixels)
                {
                    V3 ray = BuildRay(camera, bitmap.Width, bitmap.Height, pixel.Item2, pixel.Item3);
                    float backgroundAllowedStart = BackgroundAllowedStart(runtime);
                    float terrainDiagnosticStart = TerrainDiagnosticStart(runtime);

                    Hit shaderHit = FindShaderTerrain(camera.Pos, ray, terrainDiagnosticStart, 10400.0f, 8.0f);
                    Hit cpuHit = FindCpuSolid(camera.Pos, ray, terrainDiagnosticStart, 10400.0f, 4.0f);
                    float firstExpected = Math.Min(shaderHit.Found ? shaderHit.T : 1.0e20f, cpuHit.Found ? cpuHit.T : 1.0e20f);
                    string firstExpectedText = firstExpected < 1.0e19f ? F(firstExpected) : "";
                    int ring = PreferredRing(firstExpected);
                    V3 expectedPos = firstExpected < 1.0e19f
                        ? camera.Pos + ray * firstExpected
                        : new V3(0.0, 0.0, 0.0);
                    MidProbe midProbe = ProbeMidRepresentation(expectedPos, firstExpected, runtime);
                    FarProbe farProbe = ProbeFarSvoRepresentation(expectedPos, firstExpected, runtime);
                    string match = (shaderHit.Found == cpuHit.Found).ToString(CultureInfo.InvariantCulture);
                    string notes = "";
                    if (shaderHit.Found && cpuHit.Found) {
                        notes = Math.Abs(shaderHit.T - cpuHit.T) <= 16.0f ? "cpu_gpu_truth_agree" : "cpu_gpu_truth_distance_differs";
                    } else if (shaderHit.Found && !cpuHit.Found) {
                        notes = "shader_truth_solid_cpu_air";
                    } else if (!shaderHit.Found && cpuHit.Found) {
                        notes = "cpu_truth_solid_shader_air";
                    } else {
                        notes = "both_truth_air_to_10400";
                    }
                    string dominantBucket = ClassifyDominantBucket(cpuHit, shaderHit, midProbe, farProbe);

                    writer.WriteLine(string.Join(",",
                        frame.ToString(CultureInfo.InvariantCulture),
                        Csv(pixel.Item1),
                        pixel.Item2.ToString(CultureInfo.InvariantCulture),
                        pixel.Item3.ToString(CultureInfo.InvariantCulture),
                        pixel.Item4.R.ToString(CultureInfo.InvariantCulture),
                        pixel.Item4.G.ToString(CultureInfo.InvariantCulture),
                        pixel.Item4.B.ToString(CultureInfo.InvariantCulture),
                        F(camera.Pos.X), F(camera.Pos.Y), F(camera.Pos.Z),
                        F(ray.X), F(ray.Y), F(ray.Z),
                        F(backgroundAllowedStart),
                        F(terrainDiagnosticStart),
                        shaderHit.Found ? F(shaderHit.T) : "",
                        (shaderHit.Found && shaderHit.T < backgroundAllowedStart).ToString(CultureInfo.InvariantCulture),
                        shaderHit.Found ? F(shaderHit.Pos.X) : "",
                        shaderHit.Found ? F(shaderHit.Pos.Y) : "",
                        shaderHit.Found ? F(shaderHit.Pos.Z) : "",
                        shaderHit.Found ? F(shaderHit.Height) : "",
                        cpuHit.Found ? F(cpuHit.T) : "",
                        (cpuHit.Found && cpuHit.T < backgroundAllowedStart).ToString(CultureInfo.InvariantCulture),
                        cpuHit.Found ? F(cpuHit.Pos.X) : "",
                        cpuHit.Found ? F(cpuHit.Pos.Y) : "",
                        cpuHit.Found ? F(cpuHit.Pos.Z) : "",
                        cpuHit.Found ? F(cpuHit.Height) : "",
                        Csv(cpuHit.Found ? cpuHit.Material : ""),
                        match,
                        firstExpectedText,
                        (firstExpected < backgroundAllowedStart).ToString(CultureInfo.InvariantCulture),
                        ring >= 0 ? ring.ToString(CultureInfo.InvariantCulture) : "",
                        midProbe.Valid ? F(midProbe.CellSize) : "",
                        midProbe.Valid ? midProbe.BrickX.ToString(CultureInfo.InvariantCulture) : "",
                        midProbe.Valid ? midProbe.BrickY.ToString(CultureInfo.InvariantCulture) : "",
                        midProbe.Valid ? midProbe.BrickZ.ToString(CultureInfo.InvariantCulture) : "",
                        midProbe.Valid ? midProbe.RuntimeRingResident.ToString(CultureInfo.InvariantCulture) : "",
                        midProbe.Valid ? midProbe.RuntimeRingMissing.ToString(CultureInfo.InvariantCulture) : "",
                        midProbe.Valid ? F(midProbe.RuntimeRingCoveragePct) : "",
                        Csv(midProbe.Valid ? midProbe.GeneratedMaterial : ""),
                        midProbe.Valid ? midProbe.GeneratedSolid.ToString(CultureInfo.InvariantCulture) : "",
                        Csv(midProbe.Valid ? midProbe.Bucket : "no_expected_terrain"),
                        runtime.FarSvoReady.ToString(CultureInfo.InvariantCulture),
                        farProbe.Valid ? farProbe.PageX.ToString(CultureInfo.InvariantCulture) : "",
                        farProbe.Valid ? farProbe.PageZ.ToString(CultureInfo.InvariantCulture) : "",
                        farProbe.Valid ? farProbe.PageInRange.ToString(CultureInfo.InvariantCulture) : "",
                        farProbe.Valid ? farProbe.GeneratedSolid.ToString(CultureInfo.InvariantCulture) : "",
                        Csv(dominantBucket),
                        Csv("offline_unavailable"),
                        Csv("offline_unavailable"),
                        Csv("offline_unavailable"),
                        Csv(notes)));
                }
            }
        }
    }

    struct Hit
    {
        public bool Found;
        public float T;
        public V3 Pos;
        public float Height;
        public string Material;
    }

    struct RuntimeState
    {
        public bool FarSvoReady;
        public float ExactNear;
        public float SurfaceRasterMax;
        public float MidStartDistance;
        public float MidEndDistance;
        public int[] MidRingResident;
        public int[] MidRingMissing;
    }

    struct MidProbe
    {
        public bool Valid;
        public int Ring;
        public float CellSize;
        public int BrickX;
        public int BrickY;
        public int BrickZ;
        public int RuntimeRingResident;
        public int RuntimeRingMissing;
        public float RuntimeRingCoveragePct;
        public string GeneratedMaterial;
        public bool GeneratedSolid;
        public string Bucket;
    }

    struct FarProbe
    {
        public bool Valid;
        public int PageX;
        public int PageZ;
        public bool PageInRange;
        public bool GeneratedSolid;
    }

    static Camera ParseCamera(string logPath, int frame)
    {
        string text = File.ReadAllText(logPath);
        Regex re = new Regex(@"PERF_SPARSE_WALK frame=" + frame + @".*cam=\(([-0-9.]+),([-0-9.]+),([-0-9.]+)\).*yaw=([-0-9.]+) pitch=([-0-9.]+) forward=\(([-0-9.]+),([-0-9.]+),([-0-9.]+)\)");
        Match m = re.Match(text);
        if (!m.Success)
        {
            re = new Regex(@"PERF_CAMERA_EXPOSURE frame=" + frame + @".*cam=\(([-0-9.]+),([-0-9.]+),([-0-9.]+)\).*yaw=([-0-9.]+) pitch=([-0-9.]+) forward=\(([-0-9.]+),([-0-9.]+),([-0-9.]+)\)");
            m = re.Match(text);
        }
        if (!m.Success) throw new InvalidOperationException("Could not find camera state for frame=" + frame);
        Camera c = new Camera();
        c.Pos = new V3(D(m, 1), D(m, 2), D(m, 3));
        c.Yaw = D(m, 4);
        c.Pitch = D(m, 5);
        c.Forward = Normalize(new V3(D(m, 6), D(m, 7), D(m, 8)));
        return c;
    }

    static RuntimeState ParseRuntimeState(string logPath, int frame)
    {
        string text = File.ReadAllText(logPath);
        RuntimeState state = new RuntimeState();
        state.ExactNear = ExactNear;
        state.SurfaceRasterMax = DefaultSurfaceOwnershipRadius;
        state.MidStartDistance = MidStart;
        state.MidEndDistance = MidEnd;
        state.MidRingResident = new int[MidRings];
        state.MidRingMissing = new int[MidRings];
        Regex exposureRe = new Regex(@"PERF_CAMERA_EXPOSURE frame=" + frame + @".*exactNear=([-0-9.]+) surfaceRasterMax=([-0-9.]+) midStart=([-0-9.]+) midEnd=([-0-9.]+).*farSvo=(\d+) farStage=([a-zA-Z]+)");
        Match exposure = exposureRe.Match(text);
        if (exposure.Success)
        {
            state.ExactNear = F32(exposure, 1);
            state.SurfaceRasterMax = F32(exposure, 2);
            state.MidStartDistance = F32(exposure, 3);
            state.MidEndDistance = F32(exposure, 4);
            state.FarSvoReady =
                exposure.Groups[5].Value == "1" &&
                string.Equals(exposure.Groups[6].Value, "complete", StringComparison.OrdinalIgnoreCase);
        }
        Regex perfRe = new Regex(@"PERF frame=" + frame + @"[^\r\n]*farSvo=([a-zA-Z]+)[^\r\n]*farStage=([a-zA-Z]+)[^\r\n]*farCov=([-0-9.]+)/([-0-9.]+)");
        Match perf = perfRe.Match(text);
        if (perf.Success)
        {
            state.FarSvoReady =
                string.Equals(perf.Groups[1].Value, "on", StringComparison.OrdinalIgnoreCase) &&
                string.Equals(perf.Groups[2].Value, "complete", StringComparison.OrdinalIgnoreCase);
        }

        Regex clipRe = new Regex(@"PERF_SPARSE_CLIPMAP frame=" + frame + @"[^\r\n]*voxelRings=(\d+)[^\r\n]*res=([0-9/]+)[^\r\n]*miss=([0-9/]+)");
        Match clip = clipRe.Match(text);
        bool clipFromFallback = false;
        if (!clip.Success)
        {
            Regex anyClipRe = new Regex(@"PERF_SPARSE_CLIPMAP frame=(\d+)[^\r\n]*voxelRings=(\d+)[^\r\n]*res=([0-9/]+)[^\r\n]*miss=([0-9/]+)");
            Match best = Match.Empty;
            int bestFrame = -1;
            foreach (Match candidate in anyClipRe.Matches(text))
            {
                int candidateFrame;
                if (!int.TryParse(candidate.Groups[1].Value, NumberStyles.Integer, CultureInfo.InvariantCulture, out candidateFrame))
                {
                    continue;
                }
                if (candidateFrame <= frame && candidateFrame > bestFrame)
                {
                    bestFrame = candidateFrame;
                    best = candidate;
                }
            }
            clip = best;
            clipFromFallback = clip.Success;
        }
        if (clip.Success)
        {
            FillRingCounts(clip.Groups[clipFromFallback ? 3 : 2].Value, state.MidRingResident);
            FillRingCounts(clip.Groups[clipFromFallback ? 4 : 3].Value, state.MidRingMissing);
        }
        return state;
    }

    static void FillRingCounts(string text, int[] counts)
    {
        string[] parts = text.Split('/');
        for (int i = 0; i < counts.Length && i < parts.Length; ++i)
        {
            int value;
            counts[i] = int.TryParse(parts[i], NumberStyles.Integer, CultureInfo.InvariantCulture, out value)
                ? value
                : 0;
        }
    }

    static List<Tuple<string, int, int, Color>> PickHolePixels(Bitmap bitmap, int maxRows)
    {
        PixelRegion[] regions = new PixelRegion[] {
            new PixelRegion("right_upper_arch", 1488, 300, 1812, 420),
            new PixelRegion("right_lower_blue_gap", 1576, 568, 1640, 600),
            new PixelRegion("middle_arch", 960, 376, 1120, 424),
            new PixelRegion("left_middle_gap", 520, 330, 700, 410)
        };
        List<Tuple<string, int, int, Color>> result = new List<Tuple<string, int, int, Color>>();
        int perRegion = Math.Max(1, maxRows / regions.Length);
        foreach (PixelRegion r in regions)
        {
            int added = 0;
            for (int y = r.Y0; y < r.Y1 && added < perRegion; y += 8)
            {
                for (int x = r.X0; x < r.X1 && added < perRegion; x += 8)
                {
                    Color c = bitmap.GetPixel(x, y);
                    if (IsHoleSkyLike(c))
                    {
                        result.Add(Tuple.Create(r.Name, x, y, c));
                        added++;
                    }
                }
            }
        }
        return result;
    }

    static List<Tuple<string, int, int, Color>> PickExplicitPixels(Bitmap bitmap, string samplePoints, int maxRows)
    {
        List<Tuple<string, int, int, Color>> result = new List<Tuple<string, int, int, Color>>();
        string[] points = samplePoints.Split(new char[] { ';', '|' }, StringSplitOptions.RemoveEmptyEntries);
        foreach (string point in points)
        {
            if (result.Count >= Math.Max(1, maxRows))
            {
                break;
            }
            string[] parts = point.Split(new char[] { ',', ':' }, StringSplitOptions.RemoveEmptyEntries);
            if (parts.Length < 2)
            {
                continue;
            }
            int x, y;
            if (!Int32.TryParse(parts[0].Trim(), NumberStyles.Integer, CultureInfo.InvariantCulture, out x) ||
                !Int32.TryParse(parts[1].Trim(), NumberStyles.Integer, CultureInfo.InvariantCulture, out y))
            {
                continue;
            }
            x = Math.Max(0, Math.Min(bitmap.Width - 1, x));
            y = Math.Max(0, Math.Min(bitmap.Height - 1, y));
            result.Add(Tuple.Create("explicit", x, y, bitmap.GetPixel(x, y)));
        }
        return result;
    }

    static List<Tuple<string, int, int, Color>> PickCurrentHolePixels(Bitmap bitmap, int maxRows)
    {
        List<Tuple<string, int, int, Color>> candidates = new List<Tuple<string, int, int, Color>>();
        int step = 8;
        int y0 = Math.Max(24, bitmap.Height / 7);
        int y1 = Math.Min(bitmap.Height - 96, (bitmap.Height * 2) / 3);
        for (int y = y0; y < y1; y += step)
        {
            for (int x = 24; x < bitmap.Width - 24; x += step)
            {
                Color c = bitmap.GetPixel(x, y);
                if (!IsHoleSkyLike(c) || !HasTerrainContext(bitmap, x, y))
                {
                    continue;
                }
                candidates.Add(Tuple.Create("auto_skyline_gap", x, y, c));
            }
        }

        List<Tuple<string, int, int, Color>> result = new List<Tuple<string, int, int, Color>>();
        if (candidates.Count == 0)
        {
            return result;
        }
        int stride = Math.Max(1, candidates.Count / Math.Max(1, maxRows));
        for (int i = 0; i < candidates.Count && result.Count < maxRows; i += stride)
        {
            result.Add(candidates[i]);
        }
        return result;
    }

    static List<Tuple<string, int, int, Color>> PickDebugMissPixels(Bitmap bitmap, int maxRows)
    {
        List<Tuple<string, int, int, Color>> candidates = new List<Tuple<string, int, int, Color>>();
        int step = 4;
        for (int y = 24; y < bitmap.Height - 24; y += step)
        {
            for (int x = 24; x < bitmap.Width - 24; x += step)
            {
                Color c = bitmap.GetPixel(x, y);
                bool debugMiss =
                    Math.Abs(c.R - 255) <= 8 &&
                    Math.Abs(c.G - 13) <= 14 &&
                    Math.Abs(c.B - 5) <= 12;
                if (debugMiss)
                {
                    candidates.Add(Tuple.Create("debug_owner_miss", x, y, c));
                }
            }
        }

        List<Tuple<string, int, int, Color>> result = new List<Tuple<string, int, int, Color>>();
        if (candidates.Count == 0)
        {
            return result;
        }
        int stride = Math.Max(1, candidates.Count / Math.Max(1, maxRows));
        for (int i = 0; i < candidates.Count && result.Count < maxRows; i += stride)
        {
            result.Add(candidates[i]);
        }
        return result;
    }

    static bool HasTerrainContext(Bitmap bitmap, int x, int y)
    {
        int terrainLike = 0;
        int skyLike = 0;
        for (int oy = -32; oy <= 32; oy += 16)
        {
            for (int ox = -48; ox <= 48; ox += 16)
            {
                if (ox == 0 && oy == 0)
                {
                    continue;
                }
                int sx = Math.Max(0, Math.Min(bitmap.Width - 1, x + ox));
                int sy = Math.Max(0, Math.Min(bitmap.Height - 1, y + oy));
                Color sample = bitmap.GetPixel(sx, sy);
                if (IsHoleSkyLike(sample))
                {
                    ++skyLike;
                }
                else if (IsTerrainLike(sample))
                {
                    ++terrainLike;
                }
            }
        }
        return terrainLike >= 3 && skyLike >= 2;
    }

    static bool IsTerrainLike(Color c)
    {
        if (c.B > c.R + 12 && c.B > c.G + 4)
        {
            return false;
        }
        int max = Math.Max(c.R, Math.Max(c.G, c.B));
        int min = Math.Min(c.R, Math.Min(c.G, c.B));
        bool stone = max - min < 54 && c.R >= 54 && c.R <= 180 && c.G >= 54 && c.G <= 180 && c.B >= 48 && c.B <= 172;
        bool green = c.G > c.R + 8 && c.G > c.B + 4 && c.G >= 70;
        bool dirt = c.R >= 70 && c.G >= 55 && c.B <= 115 && c.R >= c.B + 8;
        return stone || green || dirt;
    }

    static bool IsHoleSkyLike(Color c)
    {
        bool reasonBlue = Math.Abs(c.R - 46) <= 16 && Math.Abs(c.G - 107) <= 16 && Math.Abs(c.B - 242) <= 18;
        bool reasonPurple = Math.Abs(c.R - 122) <= 16 && Math.Abs(c.G - 41) <= 16 && Math.Abs(c.B - 255) <= 18;
        // Normal water is also blue, but much darker/red-poor in these
        // captures. Keep the automatic hole selector on actual sky/air gaps
        // so shoreline audits do not contaminate terrain-sky classification.
        bool mutedSky =
            c.R >= 95 &&
            c.G >= 115 &&
            c.B >= 135 &&
            c.B > c.R + 12 &&
            c.B > c.G + 4;
        return reasonBlue || reasonPurple || mutedSky;
    }

    static V3 BuildRay(Camera camera, int width, int height, int px, int py)
    {
        const double fov = 60.0 * Math.PI / 180.0;
        double aspect = (double)width / (double)height;
        double ndcX = (((double)px + 0.5) / (double)width) * 2.0 - 1.0;
        double ndcY = -(((((double)py + 0.5) / (double)height) * 2.0) - 1.0);
        double tanHalf = Math.Tan(fov * 0.5);
        V3 right = Normalize(Cross(camera.Forward, new V3(0.0, 1.0, 0.0)));
        V3 up = Normalize(Cross(right, camera.Forward));
        return Normalize(camera.Forward + right * (ndcX * tanHalf * aspect) + up * (ndcY * tanHalf));
    }

    static float BackgroundAllowedStart(RuntimeState runtime)
    {
        float exactNear = runtime.ExactNear > 0.0f ? runtime.ExactNear : ExactNear;
        float surfaceRasterMax = runtime.SurfaceRasterMax > 0.0f ? runtime.SurfaceRasterMax : DefaultSurfaceOwnershipRadius;
        float protectedBand = Math.Max(exactNear + 768.0f, 1536.0f);
        float lowAltitudeProtected = Math.Max(exactNear, Math.Min(surfaceRasterMax, protectedBand));
        return lowAltitudeProtected + 8.0f;
    }

    static float TerrainDiagnosticStart(RuntimeState runtime)
    {
        float exactNear = runtime.ExactNear > 0.0f ? runtime.ExactNear : ExactNear;
        float midStart = runtime.MidStartDistance > 0.0f ? runtime.MidStartDistance : MidStart;
        return Math.Max(160.0f, Math.Min(midStart, exactNear + 8.0f));
    }

    static Hit FindShaderTerrain(V3 origin, V3 ray, float start, float end, float step)
    {
        Hit h = new Hit();
        for (float t = start; t <= end; t += step)
        {
            V3 p = origin + ray * t;
            float height = FarTerrainTruthHeight((float)p.X, (float)p.Z);
            if (p.Y <= height)
            {
                h.Found = true; h.T = t; h.Pos = p; h.Height = height; h.Material = "shader_solid";
                return h;
            }
        }
        return h;
    }

    static Hit FindCpuSolid(V3 origin, V3 ray, float start, float end, float step)
    {
        Hit h = new Hit();
        for (float t = start; t <= end; t += step)
        {
            V3 p = origin + ray * t;
            int x = FloorToInt(p.X);
            int y = FloorToInt(p.Y);
            int z = FloorToInt(p.Z);
            float height = CpuHeightAt(x, z);
            float relief = CpuSurfaceReliefAtWithCenter(x, z, height, 4);
            string material = CpuMaterialAt(x, y, z, height, relief);
            if (material != "Air")
            {
                h.Found = true; h.T = t; h.Pos = p; h.Height = height; h.Material = material;
                return h;
            }
        }
        return h;
    }

    static int PreferredRing(float t)
    {
        if (t >= 1.0e19f || t < MidStart || t > MidEnd) return -1;
        float u = Math.Max(0.0f, Math.Min(0.9999f, (t - MidStart) / Math.Max(MidEnd - MidStart, 1.0f)));
        return Math.Min((int)Math.Floor(u * MidRings), MidRings - 1);
    }

    static float MidRingCellSize(int ring)
    {
        if (ring < 0) return MidCell;
        return MidCell * (float)(1 << Math.Min(ring, 12));
    }

    static MidProbe ProbeMidRepresentation(V3 pos, float t, RuntimeState runtime)
    {
        MidProbe probe = new MidProbe();
        int ring = PreferredRing(t);
        if (ring < 0)
        {
            probe.Bucket = "outside_mid_range";
            return probe;
        }

        probe.Valid = true;
        probe.Ring = ring;
        probe.CellSize = MidRingCellSize(ring);
        float brickWorldSize = probe.CellSize * 16.0f;
        probe.BrickX = (int)Math.Floor(pos.X / brickWorldSize);
        probe.BrickY = (int)Math.Floor(pos.Y / brickWorldSize);
        probe.BrickZ = (int)Math.Floor(pos.Z / brickWorldSize);
        if (runtime.MidRingResident != null && ring < runtime.MidRingResident.Length)
        {
            probe.RuntimeRingResident = runtime.MidRingResident[ring];
            probe.RuntimeRingMissing = runtime.MidRingMissing[ring];
            int total = probe.RuntimeRingResident + probe.RuntimeRingMissing;
            probe.RuntimeRingCoveragePct = total > 0 ? (probe.RuntimeRingResident * 100.0f) / total : 0.0f;
        }

        probe.GeneratedMaterial = MidGeneratedMaterialAt(pos, probe.CellSize);
        probe.GeneratedSolid = probe.GeneratedMaterial != "Air";
        if (!probe.GeneratedSolid)
        {
            probe.Bucket = "mid_generated_air";
        }
        else if (probe.RuntimeRingMissing > 0)
        {
            probe.Bucket = "mid_ring_partially_missing";
        }
        else
        {
            probe.Bucket = "mid_generated_solid_ring_resident";
        }
        return probe;
    }

    static FarProbe ProbeFarSvoRepresentation(V3 pos, float t, RuntimeState runtime)
    {
        FarProbe probe = new FarProbe();
        if (t >= 1.0e19f) return probe;
        probe.Valid = true;
        const float pageSize = 1024.0f;
        const int pageRadius = 6;
        probe.PageX = (int)Math.Floor(pos.X / pageSize);
        probe.PageZ = (int)Math.Floor(pos.Z / pageSize);
        probe.PageInRange =
            probe.PageX >= -pageRadius && probe.PageX <= pageRadius &&
            probe.PageZ >= -pageRadius && probe.PageZ <= pageRadius &&
            runtime.FarSvoReady;
        float height = CpuHeightAtFloat((float)pos.X, (float)pos.Z);
        probe.GeneratedSolid = pos.Y <= height || (pos.Y <= SeaLevel && height < SeaLevel);
        return probe;
    }

    static string ClassifyDominantBucket(Hit cpuHit, Hit shaderHit, MidProbe midProbe, FarProbe farProbe)
    {
        if (!cpuHit.Found && !shaderHit.Found) return "true_procedural_air";
        if (cpuHit.Found && !shaderHit.Found) return "cpu_solid_shader_air";
        if (!cpuHit.Found && shaderHit.Found) return "shader_solid_cpu_air";
        if (cpuHit.Found && shaderHit.Found)
        {
            if (midProbe.Valid && !midProbe.GeneratedSolid) return "cpu_solid_mid_representation_air";
            if (farProbe.Valid && !farProbe.GeneratedSolid) return "cpu_solid_far_representation_air";
            if (midProbe.Valid && midProbe.RuntimeRingMissing > 0) return "cpu_shader_solid_mid_ring_partially_missing";
            if (farProbe.Valid && !farProbe.PageInRange) return "cpu_shader_solid_far_svo_unavailable";
            return "cpu_shader_solid_representation_expected_solid";
        }
        return "unclassified";
    }

    static string MidGeneratedMaterialAt(V3 pos, float cellSize)
    {
        int cellX = (int)Math.Floor(pos.X / cellSize);
        int cellY = (int)Math.Floor(pos.Y / cellSize);
        int cellZ = (int)Math.Floor(pos.Z / cellSize);
        int minX = (int)Math.Floor(cellX * cellSize);
        int minY = (int)Math.Floor(cellY * cellSize);
        int minZ = (int)Math.Floor(cellZ * cellSize);
        int maxX = (int)Math.Ceiling((cellX + 1) * cellSize) - 1;
        int maxY = (int)Math.Ceiling((cellY + 1) * cellSize) - 1;
        int maxZ = (int)Math.Ceiling((cellZ + 1) * cellSize) - 1;
        int sampleX = (int)Math.Round((cellX + 0.5) * cellSize);
        int sampleY = (int)Math.Round((cellY + 0.5) * cellSize);
        int sampleZ = (int)Math.Round((cellZ + 0.5) * cellSize);

        float centerHeight = CpuHeightAt(sampleX, sampleZ);
        float maxFootprintHeight = centerHeight;
        int maxFootprintX = sampleX;
        int maxFootprintZ = sampleZ;
        int[,] offsets = new int[,] {
            { 0, 0 }, { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 },
            { -1, -1 }, { 1, -1 }, { -1, 1 }, { 1, 1 }
        };
        for (int i = 0; i < offsets.GetLength(0); ++i)
        {
            int sx = sampleX + offsets[i, 0] * Math.Max(1, (int)Math.Round(cellSize));
            int sz = sampleZ + offsets[i, 1] * Math.Max(1, (int)Math.Round(cellSize));
            float h = CpuHeightAt(sx, sz);
            if (h > maxFootprintHeight)
            {
                maxFootprintHeight = h;
                maxFootprintX = sx;
                maxFootprintZ = sz;
            }
        }
        for (int sampleZIndex = 0; sampleZIndex < 5; ++sampleZIndex)
        {
            int sz = minZ + (int)(((long)(maxZ - minZ) * sampleZIndex + 2L) / 4L);
            for (int sampleXIndex = 0; sampleXIndex < 5; ++sampleXIndex)
            {
                int sx = minX + (int)(((long)(maxX - minX) * sampleXIndex + 2L) / 4L);
                float h = CpuHeightAt(sx, sz);
                if (h > maxFootprintHeight)
                {
                    maxFootprintHeight = h;
                    maxFootprintX = sx;
                    maxFootprintZ = sz;
                }
            }
        }

        if (maxY <= TerrainMinY + 2.0f) return "Bedrock";
        if (centerHeight < SeaLevel && minY <= SeaLevel && maxY > (int)Math.Floor(centerHeight)) return "Water";
        if ((float)minY <= centerHeight) return CpuMaterialAt(sampleX, Math.Min(sampleY, (int)Math.Floor(centerHeight)), sampleZ, centerHeight, CpuSurfaceReliefAtWithCenter(sampleX, sampleZ, centerHeight, 4));
        if (cellSize > 1.5f && (float)minY <= maxFootprintHeight)
        {
            int topY = (int)Math.Floor(maxFootprintHeight);
            return CpuMaterialAt(maxFootprintX, Math.Min(sampleY, topY), maxFootprintZ, maxFootprintHeight, CpuSurfaceReliefAtWithCenter(maxFootprintX, maxFootprintZ, maxFootprintHeight, 4));
        }
        return "Air";
    }

    static float FarFallbackCellSize(float distance)
    {
        float t = Saturate((distance - 900.0f) / 6500.0f);
        if (t < 0.18f) return 8.0f;
        if (t < 0.42f) return 12.0f;
        if (t < 0.68f) return 18.0f;
        return 28.0f;
    }

    static float FarTerrainHeightVoxelized(float x, float z, float distance)
    {
        float cell = FarFallbackCellSize(distance);
        float sx = (float)((Math.Floor(x / cell) + 0.5) * cell);
        float sz = (float)((Math.Floor(z / cell) + 0.5) * cell);
        float raw = CpuHeightAtFloat(sx, sz);
        float vertical = Math.Max(4.0f, cell * 0.75f);
        return (float)Math.Ceiling(raw / vertical) * vertical;
    }

    static float FarTerrainTruthHeight(float x, float z)
    {
        return CpuHeightAtFloat(x, z);
    }

    static float CpuHeightAt(int x, int z)
    {
        return CpuHeightAtFloat((float)x, (float)z);
    }

    static float CpuHeightAtFloat(float x, float z)
    {
        float broad = ValueNoise2D(x * 0.0045f, z * 0.0045f, Seed + 11u);
        float ridgeSource = ValueNoise2D(x * 0.0100f + 41.0f, z * 0.0100f - 17.0f, Seed + 23u);
        float ridge = 1.0f - Math.Abs(ridgeSource);
        float detail = ValueNoise2D(x * 0.035f - 13.0f, z * 0.035f + 29.0f, Seed + 37u);
        float ridgeHeight = ridge * ridge;
        float height = -64.0f + broad * 145.0f + ridgeHeight * 150.0f + detail * 8.0f;
        float dx = x - 192.0f;
        float dz = z - 224.0f;
        float originDistance = (float)Math.Sqrt(dx * dx + dz * dz);
        float originComfort = 1.0f - Smooth01(Clamp((originDistance - 180.0f) / 520.0f, 0.0f, 1.0f));
        float publicRegionHeight = -42.0f + broad * 54.0f + ridgeHeight * 48.0f + detail * 3.0f +
            (1.0f - Smooth01(originDistance / 360.0f)) * 72.0f;
        height += (1.0f - Smooth01(originDistance / 420.0f)) * 58.0f;
        height = Lerp(height, publicRegionHeight, originComfort * 0.94f);
        float publicCapInfluence = 1.0f - Smooth01(Clamp((originDistance - 220.0f) / 420.0f, 0.0f, 1.0f));
        float publicCap = 58.0f + Smooth01(Clamp(originDistance / 640.0f, 0.0f, 1.0f)) * 114.0f;
        height = Lerp(height, Math.Min(height, publicCap), publicCapInfluence);
        float submergedBlend = 1.0f - Smooth01(Clamp((height - (SeaLevel + 28.0f)) / 86.0f, 0.0f, 1.0f));
        if (submergedBlend > 0.0f)
        {
            float shelf = (SeaLevel - 8.0f) + broad * 38.0f + ridgeHeight * 22.0f + detail * 2.0f +
                (1.0f - Smooth01(originDistance / 520.0f)) * 18.0f;
            height = Lerp(height, shelf, submergedBlend * 0.55f);
        }
        float playableBankBand =
            1.0f - Smooth01(Clamp((originDistance - 260.0f) / 980.0f, 0.0f, 1.0f));
        float lowlandUpper = 1.0f - Smooth01(Clamp((height - (SeaLevel + 96.0f)) / 120.0f, 0.0f, 1.0f));
        float lowlandFloor = Smooth01(Clamp((height - (SeaLevel - 40.0f)) / 64.0f, 0.0f, 1.0f));
        float playableBankBlend = playableBankBand * lowlandUpper * lowlandFloor * 0.64f;
        if (playableBankBlend > 0.0f)
        {
            float playableShelfHeight =
                (SeaLevel + 18.0f) + broad * 28.0f + ridgeHeight * 10.0f + detail * 1.5f +
                (1.0f - Smooth01(Clamp(originDistance / 460.0f, 0.0f, 1.0f))) * 42.0f;
            height = Lerp(height, playableShelfHeight, playableBankBlend);
        }
        float publicBasinBand =
            Smooth01(Clamp((originDistance - 360.0f) / 240.0f, 0.0f, 1.0f)) *
            (1.0f - Smooth01(Clamp((originDistance - 1700.0f) / 760.0f, 0.0f, 1.0f))) *
            Smooth01(Clamp((height - (SeaLevel - 38.0f)) / 56.0f, 0.0f, 1.0f)) *
            (1.0f - Smooth01(Clamp((height - (SeaLevel + 180.0f)) / 140.0f, 0.0f, 1.0f)));
        float publicBasinFloor = (SeaLevel - 12.0f) + broad * 2.0f + detail * 0.35f;
        if (publicBasinBand > 0.0f)
        {
            height = Lerp(height, Math.Min(height, publicBasinFloor), publicBasinBand * 0.80f);
        }
        float backdropNoise = ValueNoise2D(x * 0.0018f + 19.0f, z * 0.0018f - 31.0f, Seed + 211u);
        float backdropRidgeSource = ValueNoise2D(x * 0.0032f - 71.0f, z * 0.0032f + 43.0f, Seed + 227u);
        float backdropRidge = 1.0f - Math.Abs(backdropRidgeSource);
        float backdropBreakup = ValueNoise2D(x * 0.0075f + 203.0f, z * 0.0075f - 167.0f, Seed + 271u);
        float backdropNotch = Smooth01(Clamp((backdropBreakup - 0.08f) / 0.58f, 0.0f, 1.0f));
        float silhouetteRidge = Clamp(backdropRidge * backdropRidge * 1.22f + backdropNoise * 0.18f, 0.0f, 1.0f);
        float backdropBand =
            Smooth01(Clamp((originDistance - 1360.0f) / 700.0f, 0.0f, 1.0f)) *
            (1.0f - Smooth01(Clamp((originDistance - 5200.0f) / 1200.0f, 0.0f, 1.0f)));
        float northBackdrop = Smooth01(Clamp((z - 1180.0f) / 900.0f, 0.0f, 1.0f));
        float sideBackdrop = Smooth01(Clamp((Math.Abs(x - 192.0f) - 820.0f) / 980.0f, 0.0f, 1.0f));
        float backdropInfluence =
            backdropBand *
            Clamp(northBackdrop + sideBackdrop * 0.58f, 0.0f, 1.0f) *
            Smooth01(silhouetteRidge) *
            (0.46f + backdropNotch * 0.54f);
        float backdropHeight =
            248.0f +
            backdropBand * 160.0f +
            silhouetteRidge * 186.0f +
            backdropNoise * 26.0f;
        height = Lerp(height, Math.Max(height, backdropHeight), backdropInfluence * 0.70f);
        float westCorridor = Smooth01(Clamp((192.0f - x - 520.0f) / 820.0f, 0.0f, 1.0f));
        float eastCorridor = Smooth01(Clamp((x - 192.0f - 520.0f) / 820.0f, 0.0f, 1.0f));
        float southBlend = Smooth01(Clamp((360.0f - z) / 1200.0f, 0.0f, 1.0f));
        float westNorthBlend = Smooth01(Clamp((z - 360.0f) / 920.0f, 0.0f, 1.0f));
        float routeDistanceBand =
            Smooth01(Clamp((originDistance - 780.0f) / 420.0f, 0.0f, 1.0f)) *
            (1.0f - Smooth01(Clamp((originDistance - 4300.0f) / 1200.0f, 0.0f, 1.0f)));
        float routeCorridor = routeDistanceBand * Clamp(
            westCorridor * (0.50f + southBlend * 0.42f + westNorthBlend * 0.30f) +
            eastCorridor * southBlend,
            0.0f,
            1.0f);
        float routeRidgeNoiseA = ValueNoise2D(x * 0.0024f + 113.0f, z * 0.0024f - 89.0f, Seed + 251u);
        float routeRidgeNoiseB = ValueNoise2D(x * 0.0068f - 37.0f, z * 0.0068f + 151.0f, Seed + 263u);
        float routeBreakup = ValueNoise2D(x * 0.0110f - 211.0f, z * 0.0110f + 73.0f, Seed + 281u);
        float routeNotch = Smooth01(Clamp((routeBreakup - 0.02f) / 0.60f, 0.0f, 1.0f));
        float routeRidge =
            Clamp(
                0.26f +
                (1.0f - Math.Abs(routeRidgeNoiseA)) * 0.58f +
                routeRidgeNoiseB * 0.16f,
                0.0f,
                1.0f);
        float routeBackdropHeight =
            272.0f +
            routeDistanceBand * 104.0f +
            routeRidge * 218.0f;
        height = Lerp(height, Math.Max(height, routeBackdropHeight), routeCorridor * routeRidge * routeNotch * 0.68f);
        return Clamp(height, TerrainMinY, TerrainMaxY);
    }

    static float CpuSurfaceReliefAtWithCenter(int x, int z, float center, int offset)
    {
        float min = center;
        float max = center;
        float[] samples = new float[] {
            CpuHeightAt(x - offset, z),
            CpuHeightAt(x + offset, z),
            CpuHeightAt(x, z - offset),
            CpuHeightAt(x, z + offset)
        };
        foreach (float s in samples) { min = Math.Min(min, s); max = Math.Max(max, s); }
        return max - min;
    }

    static string CpuMaterialAt(int x, int y, int z, float height, float relief)
    {
        if (y <= -332 + 2) return "Bedrock";
        if ((float)y <= height)
        {
            float depth = height - (float)y;
            bool steep = relief > 10.0f || height > 160.0f;
            bool dryIntertidal = height >= SeaLevel - 2.0f && height < SeaLevel + 6.0f;
            bool lowReliefShore = relief < 8.0f;
            if (dryIntertidal && depth < 4.0f && lowReliefShore) return "Sand";
            if (depth < 2.0f && !steep) return "Dirt";
            if (depth < 5.0f && relief < 6.0f) return "Dirt";
            return "Stone";
        }
        if (y <= SeaLevel && height < SeaLevel) return "Water";
        return "Air";
    }

    static float ValueNoise2D(float x, float z, uint seed)
    {
        int x0 = (int)Math.Floor(x);
        int z0 = (int)Math.Floor(z);
        float fx = x - (float)x0;
        float fz = z - (float)z0;
        float sx = Smooth01(fx);
        float sz = Smooth01(fz);
        float s00 = (float)(Hash3D(x0, 0, z0, seed) & 0xFFFFFFu) / 16777215.0f;
        float s10 = (float)(Hash3D(x0 + 1, 0, z0, seed) & 0xFFFFFFu) / 16777215.0f;
        float s01 = (float)(Hash3D(x0, 0, z0 + 1, seed) & 0xFFFFFFu) / 16777215.0f;
        float s11 = (float)(Hash3D(x0 + 1, 0, z0 + 1, seed) & 0xFFFFFFu) / 16777215.0f;
        float a = Lerp(s00, s10, sx);
        float b = Lerp(s01, s11, sx);
        return Lerp(a, b, sz) * 2.0f - 1.0f;
    }

    static uint Hash3D(int x, int y, int z, uint seed)
    {
        unchecked
        {
            uint h = seed ^ 2166136261u;
            h = (h ^ (uint)x) * 16777619u;
            h = (h ^ (uint)y) * 16777619u;
            h = (h ^ (uint)z) * 16777619u;
            h ^= h >> 16;
            h *= 0x7feb352du;
            h ^= h >> 15;
            h *= 0x846ca68bu;
            h ^= h >> 16;
            return h;
        }
    }

    static V3 Cross(V3 a, V3 b)
    {
        return new V3(a.Y * b.Z - a.Z * b.Y, a.Z * b.X - a.X * b.Z, a.X * b.Y - a.Y * b.X);
    }

    static V3 Normalize(V3 v)
    {
        double len = Math.Sqrt(v.X * v.X + v.Y * v.Y + v.Z * v.Z);
        if (len <= 1e-9) return new V3(0.0, 0.0, 0.0);
        return new V3(v.X / len, v.Y / len, v.Z / len);
    }

    static int FloorToInt(double v) { return (int)Math.Floor(v); }
    static float Smooth01(float value) { value = Clamp(value, 0.0f, 1.0f); return value * value * (3.0f - 2.0f * value); }
    static float Lerp(float a, float b, float t) { return a + (b - a) * t; }
    static float Clamp(float v, float lo, float hi) { return Math.Max(lo, Math.Min(hi, v)); }
    static float Saturate(float v) { return Clamp(v, 0.0f, 1.0f); }
    static double D(Match m, int i) { return double.Parse(m.Groups[i].Value, CultureInfo.InvariantCulture); }
    static float F32(Match m, int i) { return float.Parse(m.Groups[i].Value, CultureInfo.InvariantCulture); }
    static string F(double value) { return value.ToString("0.###", CultureInfo.InvariantCulture); }
    static string F(float value) { return value.ToString("0.###", CultureInfo.InvariantCulture); }
    static string Csv(string s) { return "\"" + (s ?? "").Replace("\"", "\"\"") + "\""; }
}
"@

Add-Type -AssemblyName System.Drawing
Add-Type -TypeDefinition $source -ReferencedAssemblies System.Drawing

$frameAbs = (Resolve-Path $FramePath).Path
$logAbs = (Resolve-Path $LogPath).Path
[TerrainHoleRayAudit]::Run($frameAbs, $logAbs, $OutputCsv, $Frame, $MaxRows, [bool]$AutoSelect, [bool]$DebugMissSelect, $SamplePoints)
Write-Host "Wrote $OutputCsv"
