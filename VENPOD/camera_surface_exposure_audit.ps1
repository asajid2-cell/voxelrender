param(
    [string]$LogPath = "build\captures\mid_far_ablation_fixed_frame0490_20260520\normal\normal\venpod_runtime.log",
    [string]$NormalFrame490 = "build\captures\mid_far_ablation_fixed_frame0490_20260520\normal\normal\engine_frame_0490.bmp",
    [string]$OwnerFrame490 = "build\captures\mid_far_ablation_fixed_frame0490_20260520\normal\owner\engine_frame_0490.bmp",
    [string]$ExactQualityCsv = "build\captures\exact_surface_quality_audit_frame0490_20260520\exact_surface_quality_pixels.csv",
    [string]$OutputDir = "build\captures\camera_surface_exposure_audit_frame0400_0550_20260520",
    [int]$StartFrame = 400,
    [int]$EndFrame = 550
)

$ErrorActionPreference = "Stop"

if (!(Test-Path $LogPath)) { throw "Log not found: $LogPath" }
if (!(Test-Path $NormalFrame490)) { throw "Normal frame not found: $NormalFrame490" }
if (!(Test-Path $OwnerFrame490)) { throw "Owner frame not found: $OwnerFrame490" }

$source = @"
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.RegularExpressions;

public static class CameraSurfaceExposureAudit
{
    const float SeaLevel = -48.0f;
    const uint Seed = 12345u;

    struct FrameState
    {
        public int Frame;
        public float CamX, CamY, CamZ;
        public float FeetX, FeetY, FeetZ;
        public float Yaw, Pitch;
        public float FwdX, FwdY, FwdZ;
        public string BodyColl;
        public bool HasLoggedTerrain;
        public float LoggedTerrainH;
        public float LoggedCameraAbove;
        public float LoggedFeetAbove;
    }

    struct V3
    {
        public double X, Y, Z;
        public V3(double x, double y, double z) { X = x; Y = y; Z = z; }
        public static V3 operator +(V3 a, V3 b) { return new V3(a.X + b.X, a.Y + b.Y, a.Z + b.Z); }
        public static V3 operator *(V3 a, double s) { return new V3(a.X * s, a.Y * s, a.Z * s); }
    }

    public static void Run(string logPath, string normalFrame, string ownerFrame, string exactQualityCsv, string outputDir, int startFrame, int endFrame)
    {
        Directory.CreateDirectory(outputDir);
        List<FrameState> frames = ParseFrames(logPath)
            .Where(f => f.Frame >= startFrame && f.Frame <= endFrame)
            .OrderBy(f => f.Frame)
            .ToList();
        if (frames.Count == 0) throw new InvalidOperationException("No PERF_SPARSE_WALK frames found in requested range.");

        ExactMetrics exact490 = File.Exists(exactQualityCsv) ? ReadExactMetrics(exactQualityCsv) : new ExactMetrics();
        OwnerMetrics owner490 = ReadOwnerMetrics(ownerFrame);

        string routeCsv = Path.Combine(outputDir, "camera_surface_exposure_route.csv");
        using (StreamWriter w = new StreamWriter(routeCsv, false, Encoding.UTF8))
        {
            w.WriteLine("frame,cameraX,cameraY,cameraZ,feetX,feetY,feetZ,terrainHeightAtCamera,terrainHeightAtFeet,heightAboveTerrainCamera,heightAboveTerrainFeet,insideTerrainCamera,insideTerrainFeet,nearestSupportY,supportGapFeet,waterlineProximity,yaw,pitch,forwardX,forwardY,forwardZ,bodyColl,exactSurfacePct,sideInteriorPct,medianExactHitDist,medianDepthBelowHeight,likelyCause");
            foreach (FrameState f in frames)
            {
                float hCam = TerrainHeightAt(f);
                float hFeet = hCam;
                float surfCam = Math.Max(hCam, SeaLevel);
                float surfFeet = Math.Max(hFeet, SeaLevel);
                float camAbove = f.HasLoggedTerrain ? f.LoggedCameraAbove : f.CamY - surfCam;
                float feetAbove = f.HasLoggedTerrain ? f.LoggedFeetAbove : f.FeetY - surfFeet;
                bool insideCam = f.CamY <= surfCam - 0.25f;
                bool insideFeet = f.FeetY <= surfFeet - 0.25f;
                int supportY = (int)Math.Floor(surfFeet);
                float waterline = Math.Abs(hFeet - SeaLevel);
                bool hasScreen = f.Frame == 490;
                string cause = LikelyCause(camAbove, feetAbove, insideCam, insideFeet, waterline, hasScreen ? exact490.SideInteriorPct : -1.0f);
                w.WriteLine(String.Join(",",
                    f.Frame.ToString(CultureInfo.InvariantCulture),
                    Num(f.CamX), Num(f.CamY), Num(f.CamZ),
                    Num(f.FeetX), Num(f.FeetY), Num(f.FeetZ),
                    Num(hCam), Num(hFeet),
                    Num(camAbove), Num(feetAbove),
                    insideCam ? "true" : "false",
                    insideFeet ? "true" : "false",
                    supportY.ToString(CultureInfo.InvariantCulture),
                    Num(f.FeetY - supportY),
                    Num(waterline),
                    Num(f.Yaw), Num(f.Pitch),
                    Num(f.FwdX), Num(f.FwdY), Num(f.FwdZ),
                    Csv(f.BodyColl),
                    hasScreen ? Num(owner490.ExactPct) : "",
                    hasScreen ? Num(exact490.SideInteriorPct) : "",
                    hasScreen ? Num(exact490.MedianDistance) : "",
                    hasScreen ? Num(exact490.MedianDepth) : "",
                    Csv(cause)));
            }
        }

        string summaryCsv = Path.Combine(outputDir, "diagnosis_table.csv");
        WriteSummary(frames, routeCsv, summaryCsv, owner490, exact490);
        WriteRouteMap(frames, Path.Combine(outputDir, "camera_path_height_map.bmp"));
        WriteFaceDebug490(logPath, normalFrame, ownerFrame, Path.Combine(outputDir, "exact_face_type_debug_0490.bmp"));
    }

    sealed class ExactMetrics
    {
        public float SideInteriorPct;
        public float MedianDistance;
        public float MedianDepth;
        public int Samples;
    }

    sealed class OwnerMetrics
    {
        public float ExactPct;
        public float MidPct;
        public float FarPct;
        public float SkyPct;
    }

    static ExactMetrics ReadExactMetrics(string path)
    {
        string[] lines = File.ReadAllLines(path);
        if (lines.Length <= 1) return new ExactMetrics();
        List<string> headers = ParseCsvLine(lines[0]);
        int faceIdx = headers.IndexOf("faceType");
        int distIdx = headers.IndexOf("distance");
        int depthIdx = headers.IndexOf("depthBelowHeight");
        int total = 0, side = 0;
        List<float> distances = new List<float>();
        List<float> depths = new List<float>();
        for (int i = 1; i < lines.Length; ++i)
        {
            List<string> values = ParseCsvLine(lines[i]);
            if (values.Count <= Math.Max(faceIdx, Math.Max(distIdx, depthIdx))) continue;
            total++;
            string face = values[faceIdx];
            if (face.Contains("side") || face.Contains("interior") || face.Contains("below")) side++;
            distances.Add(ParseFloat(values[distIdx]));
            depths.Add(ParseFloat(values[depthIdx]));
        }
        return new ExactMetrics {
            Samples = total,
            SideInteriorPct = total > 0 ? side * 100.0f / total : 0.0f,
            MedianDistance = Median(distances),
            MedianDepth = Median(depths)
        };
    }

    static OwnerMetrics ReadOwnerMetrics(string ownerFrame)
    {
        using (Bitmap owner = new Bitmap(ownerFrame))
        {
            long exact = 0, mid = 0, far = 0, sky = 0, total = 0;
            int step = 2;
            for (int y = 0; y < owner.Height; y += step)
            for (int x = 0; x < owner.Width; x += step)
            {
                Color c = owner.GetPixel(x, y);
                total++;
                if (Near(c, 255,242,13,60)) exact++;
                else if (Near(c, 13,242,64,70)) mid++;
                else if (Near(c, 51,107,255,70)) far++;
                else if (Near(c, 46,107,242,70)) sky++;
            }
            return new OwnerMetrics {
                ExactPct = Pct(exact, total),
                MidPct = Pct(mid, total),
                FarPct = Pct(far, total),
                SkyPct = Pct(sky, total)
            };
        }
    }

    static void WriteSummary(List<FrameState> frames, string routeCsv, string summaryCsv, OwnerMetrics owner490, ExactMetrics exact490)
    {
        float minCamAbove = Single.MaxValue;
        float minFeetAbove = Single.MaxValue;
        int insideCamFrames = 0;
        int insideFeetFrames = 0;
        int nearWaterFrames = 0;
        foreach (FrameState f in frames)
        {
                float rawHeight = TerrainHeightAt(f);
                float hCam = Math.Max(rawHeight, SeaLevel);
                float hFeet = hCam;
                float camAbove = f.HasLoggedTerrain ? f.LoggedCameraAbove : f.CamY - hCam;
                float feetAbove = f.HasLoggedTerrain ? f.LoggedFeetAbove : f.FeetY - hFeet;
            minCamAbove = Math.Min(minCamAbove, camAbove);
            minFeetAbove = Math.Min(minFeetAbove, feetAbove);
            if (f.CamY <= hCam - 0.25f) insideCamFrames++;
            if (f.FeetY <= hFeet - 0.25f) insideFeetFrames++;
            if (Math.Abs(rawHeight - SeaLevel) < 12.0f) nearWaterFrames++;
        }
        using (StreamWriter w = new StreamWriter(summaryCsv, false, Encoding.UTF8))
        {
            w.WriteLine("problem_region,frames,samples,dominant_state,evidence,likely_cause,evidence_artifact");
            w.WriteLine(String.Join(",",
                Csv("route_camera_height"),
                frames.Count.ToString(CultureInfo.InvariantCulture),
                Csv("frames_" + frames.First().Frame + "_" + frames.Last().Frame),
                Csv("camera_not_inside_but_feet_close_to_generated_surface"),
                Csv("minCameraAbove=" + Num(minCamAbove) + "; minFeetAbove=" + Num(minFeetAbove) + "; insideCameraFrames=" + insideCamFrames + "; insideFeetFrames=" + insideFeetFrames),
                Csv(minCamAbove > 2.0f ? "not a camera-inside-terrain failure; likely looking across close generated side faces" : "camera/path may enter terrain"),
                Csv(routeCsv)));
            string frame490State = exact490.SideInteriorPct > 50.0f
                ? "exact_sparse_surface_side_interior_dominant"
                : "exact_sparse_surface_top_or_bank_dominant";
            string frame490Cause = exact490.SideInteriorPct > 50.0f
                ? "close exact sparse side/interior faces dominate the sampled bad pixels"
                : "previous close exact side/interior domination is gone in the sampled bad pixels";
            w.WriteLine(String.Join(",",
                Csv("frame_490_screen"),
                "1",
                exact490.Samples.ToString(CultureInfo.InvariantCulture),
                Csv(frame490State),
                Csv("exactPct=" + Num(owner490.ExactPct) + "; sideInteriorPct=" + Num(exact490.SideInteriorPct) + "; medianHitDist=" + Num(exact490.MedianDistance) + "; medianDepth=" + Num(exact490.MedianDepth)),
                Csv(frame490Cause),
                Csv("exact_face_type_debug_0490.bmp; exact_surface_quality_pixels.csv")));
            w.WriteLine(String.Join(",",
                Csv("shoreline_proximity"),
                frames.Count.ToString(CultureInfo.InvariantCulture),
                nearWaterFrames.ToString(CultureInfo.InvariantCulture),
                Csv("route_near_waterline"),
                Csv("nearWaterlineFrames=" + nearWaterFrames),
                Csv(nearWaterFrames > 0 ? "shoreline route can expose banks/side faces in front of water" : "not primarily waterline proximity"),
                Csv(routeCsv)));
        }
    }

    static void WriteRouteMap(List<FrameState> frames, string outputPath)
    {
        int width = 900, height = 700;
        float minX = frames.Min(f => f.CamX) - 40.0f, maxX = frames.Max(f => f.CamX) + 40.0f;
        float minZ = frames.Min(f => f.CamZ) - 40.0f, maxZ = frames.Max(f => f.CamZ) + 40.0f;
        using (Bitmap image = new Bitmap(width, height))
        using (Graphics g = Graphics.FromImage(image))
        {
            g.Clear(Color.FromArgb(20, 24, 28));
            for (int py = 0; py < height; py += 4)
            for (int px = 0; px < width; px += 4)
            {
                float x = minX + (maxX - minX) * px / Math.Max(1, width - 1);
                float z = maxZ - (maxZ - minZ) * py / Math.Max(1, height - 1);
                float h = HeightAt(x, z);
                int shade = (int)Math.Max(0, Math.Min(255, 70 + h * 1.2f));
                using (Brush b = new SolidBrush(Color.FromArgb(shade / 2, shade, Math.Max(20, shade / 3))))
                    g.FillRectangle(b, px, py, 4, 4);
            }
            Point? prev = null;
            foreach (FrameState f in frames)
            {
                int px = (int)((f.CamX - minX) / Math.Max(1e-3f, maxX - minX) * (width - 1));
                int py = (int)((maxZ - f.CamZ) / Math.Max(1e-3f, maxZ - minZ) * (height - 1));
                Point p = new Point(px, py);
                if (prev.HasValue) g.DrawLine(Pens.White, prev.Value, p);
                Color c = f.Frame == 490 ? Color.Red : Color.Cyan;
                using (Brush b = new SolidBrush(c)) g.FillEllipse(b, px - 4, py - 4, 8, 8);
                if (f.Frame % 30 == 0 || f.Frame == 490) g.DrawString(f.Frame.ToString(CultureInfo.InvariantCulture), SystemFonts.DefaultFont, Brushes.White, px + 6, py - 6);
                prev = p;
            }
            image.Save(outputPath);
        }
    }

    static void WriteFaceDebug490(string logPath, string normalFrame, string ownerFrame, string outputPath)
    {
        FrameState f = ParseFrames(logPath).FirstOrDefault(s => s.Frame == 490);
        if (f.Frame != 490) return;
        using (Bitmap normal = new Bitmap(normalFrame))
        using (Bitmap owner = new Bitmap(ownerFrame))
        using (Bitmap output = new Bitmap(normal.Width, normal.Height))
        using (Graphics g = Graphics.FromImage(output))
        {
            g.DrawImage(normal, 0, 0);
            int step = 4;
            for (int y = 0; y < owner.Height; y += step)
            for (int x = 0; x < owner.Width; x += step)
            {
                Color oc = owner.GetPixel(x, y);
                if (!Near(oc, 255,242,13,60)) continue;
                V3 ray = BuildRay(f, owner.Width, owner.Height, x, y);
                Hit hit = FindTerrain(new V3(f.CamX, f.CamY, f.CamZ), ray, 1.0f, 220.0f, 1.0f);
                if (!hit.Found) continue;
                N3 n = TerrainNormal((float)hit.Pos.X, (float)hit.Pos.Z, 2.0f);
                float depth = hit.Height - (float)hit.Pos.Y;
                string face = FaceType(n.Y, depth);
                Color color = face.Contains("top") ? Color.FromArgb(90, 0, 255, 0) :
                    face.Contains("deep") ? Color.FromArgb(95, 255, 0, 255) :
                    face.Contains("below") ? Color.FromArgb(95, 255, 120, 0) :
                    Color.FromArgb(90, 255, 255, 0);
                using (Brush b = new SolidBrush(color)) g.FillRectangle(b, x, y, step, step);
            }
            output.Save(outputPath);
        }
    }

    struct N3 { public float X, Y, Z; public N3(float x, float y, float z) { X = x; Y = y; Z = z; } }
    struct Hit { public bool Found; public float T, Height; public V3 Pos; }

    static Hit FindTerrain(V3 origin, V3 ray, float start, float end, float step)
    {
        for (float t = start; t <= end; t += step)
        {
            V3 p = origin + ray * t;
            float h = HeightAt((float)p.X, (float)p.Z);
            if (p.Y <= Math.Max(h, SeaLevel))
                return new Hit { Found = true, T = t, Height = h, Pos = p };
        }
        return new Hit();
    }

    static V3 BuildRay(FrameState f, int width, int height, int px, int py)
    {
        double fov = 60.0 * Math.PI / 180.0;
        double aspect = (double)width / Math.Max(1, height);
        double ndcX = (((px + 0.5) / width) * 2.0 - 1.0);
        double ndcY = -(((py + 0.5) / height) * 2.0 - 1.0);
        V3 forward = Normalize(new V3(f.FwdX, f.FwdY, f.FwdZ));
        V3 right = Normalize(Cross(forward, new V3(0, 1, 0)));
        V3 up = Normalize(Cross(right, forward));
        double tanHalf = Math.Tan(fov * 0.5);
        return Normalize(new V3(
            forward.X + right.X * ndcX * tanHalf * aspect + up.X * ndcY * tanHalf,
            forward.Y + right.Y * ndcX * tanHalf * aspect + up.Y * ndcY * tanHalf,
            forward.Z + right.Z * ndcX * tanHalf * aspect + up.Z * ndcY * tanHalf));
    }

    static List<FrameState> ParseFrames(string logPath)
    {
        string text = File.ReadAllText(logPath);
        Regex re = new Regex(@"PERF_SPARSE_WALK frame=(\d+).*?cam=\(([-0-9.]+),([-0-9.]+),([-0-9.]+)\).*?feet=\(([-0-9.]+),([-0-9.]+),([-0-9.]+)\).*?yaw=([-0-9.]+) pitch=([-0-9.]+) forward=\(([-0-9.]+),([-0-9.]+),([-0-9.]+)\) bodyColl=([0-9./-]+)(?: terrainH=([-0-9.]+) cameraAbove=([-0-9.]+) feetAbove=([-0-9.]+))?");
        List<FrameState> result = new List<FrameState>();
        foreach (Match m in re.Matches(text))
        {
            bool hasLoggedTerrain = m.Groups[14].Success && m.Groups[15].Success && m.Groups[16].Success;
            result.Add(new FrameState {
                Frame = Int32.Parse(m.Groups[1].Value, CultureInfo.InvariantCulture),
                CamX = ParseFloat(m.Groups[2].Value), CamY = ParseFloat(m.Groups[3].Value), CamZ = ParseFloat(m.Groups[4].Value),
                FeetX = ParseFloat(m.Groups[5].Value), FeetY = ParseFloat(m.Groups[6].Value), FeetZ = ParseFloat(m.Groups[7].Value),
                Yaw = ParseFloat(m.Groups[8].Value), Pitch = ParseFloat(m.Groups[9].Value),
                FwdX = ParseFloat(m.Groups[10].Value), FwdY = ParseFloat(m.Groups[11].Value), FwdZ = ParseFloat(m.Groups[12].Value),
                BodyColl = m.Groups[13].Value,
                HasLoggedTerrain = hasLoggedTerrain,
                LoggedTerrainH = hasLoggedTerrain ? ParseFloat(m.Groups[14].Value) : 0.0f,
                LoggedCameraAbove = hasLoggedTerrain ? ParseFloat(m.Groups[15].Value) : 0.0f,
                LoggedFeetAbove = hasLoggedTerrain ? ParseFloat(m.Groups[16].Value) : 0.0f
            });
        }
        return result;
    }

    static float TerrainHeightAt(FrameState f)
    {
        return f.HasLoggedTerrain ? f.LoggedTerrainH : HeightAt(f.CamX, f.CamZ);
    }

    static string LikelyCause(float camAbove, float feetAbove, bool insideCam, bool insideFeet, float waterline, float sideInteriorPct)
    {
        if (insideCam) return "camera inside generated terrain";
        if (insideFeet) return "feet/support inside generated terrain";
        if (camAbove < 4.0f) return "camera too close to generated surface";
        if (feetAbove < 2.0f && sideInteriorPct > 80.0f) return "valid low walking camera looking across close side/interior faces";
        if (waterline < 12.0f) return "near shoreline/bank exposure";
        return "route camera above terrain; inspect view direction and exact surface culling";
    }

    static string FaceType(float ny, float depth)
    {
        if (depth > 14.0f) return "deep_below_height_side_or_interior";
        if (depth > 5.0f && ny < 0.86f) return "below_height_side_face";
        if (ny > 0.86f) return "top_surface";
        if (ny > 0.42f) return "steep_top_or_bank";
        return "steep_side_face";
    }

    static N3 TerrainNormal(float x, float z, float step)
    {
        float hx0 = HeightAt(x - step, z), hx1 = HeightAt(x + step, z);
        float hz0 = HeightAt(x, z - step), hz1 = HeightAt(x, z + step);
        N3 n = new N3(-(hx1 - hx0) / (2.0f * step), 1.0f, -(hz1 - hz0) / (2.0f * step));
        float len = (float)Math.Sqrt(n.X * n.X + n.Y * n.Y + n.Z * n.Z);
        return len <= 1e-6f ? new N3(0, 1, 0) : new N3(n.X / len, n.Y / len, n.Z / len);
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
        float originComfort = 1.0f - Smooth(Clamp((originDistance - 180.0f) / 520.0f, 0.0f, 1.0f));
        float publicRegionHeight = -42.0f + broad * 54.0f + ridgeHeight * 48.0f + detail * 3.0f + (1.0f - Smooth(Clamp(originDistance / 360.0f, 0.0f, 1.0f))) * 72.0f;
        height += (1.0f - Smooth(Clamp(originDistance / 420.0f, 0.0f, 1.0f))) * 58.0f;
        height = Lerp(height, publicRegionHeight, originComfort * 0.82f);
        float publicCapInfluence = 1.0f - Smooth(Clamp((originDistance - 220.0f) / 420.0f, 0.0f, 1.0f));
        float publicCap = 76.0f + Smooth(Clamp(originDistance / 640.0f, 0.0f, 1.0f)) * 96.0f;
        height = Lerp(height, Math.Min(height, publicCap), publicCapInfluence);
        float submergedBlend = 1.0f - Smooth(Clamp((height - (SeaLevel + 28.0f)) / 86.0f, 0.0f, 1.0f));
        if (submergedBlend > 0.0f) {
            float shelf = (SeaLevel - 8.0f) + broad * 38.0f + ridgeHeight * 22.0f + detail * 2.0f + (1.0f - Smooth(Clamp(originDistance / 520.0f, 0.0f, 1.0f))) * 18.0f;
            height = Lerp(height, shelf, submergedBlend * 0.55f);
        }
        float backdropNoise = ValueNoise2D(x * 0.0018f + 19.0f, z * 0.0018f - 31.0f, Seed + 211u);
        float backdropRidgeSource = ValueNoise2D(x * 0.0032f - 71.0f, z * 0.0032f + 43.0f, Seed + 227u);
        float backdropRidge = 1.0f - Math.Abs(backdropRidgeSource);
        float backdropBreakup = ValueNoise2D(x * 0.0075f + 203.0f, z * 0.0075f - 167.0f, Seed + 271u);
        float backdropNotch = Smooth(Clamp((backdropBreakup - 0.08f) / 0.58f, 0.0f, 1.0f));
        float silhouetteRidge = Clamp(backdropRidge * backdropRidge * 1.22f + backdropNoise * 0.18f, 0.0f, 1.0f);
        float backdropBand = Smooth(Clamp((originDistance - 1450.0f) / 720.0f, 0.0f, 1.0f)) * (1.0f - Smooth(Clamp((originDistance - 5200.0f) / 1200.0f, 0.0f, 1.0f)));
        float northBackdrop = Smooth(Clamp((z - 1180.0f) / 900.0f, 0.0f, 1.0f));
        float sideBackdrop = Smooth(Clamp((Math.Abs(x - 192.0f) - 820.0f) / 980.0f, 0.0f, 1.0f));
        float backdropInfluence = backdropBand * Clamp(northBackdrop + sideBackdrop * 0.58f, 0.0f, 1.0f) * Smooth(silhouetteRidge) * (0.38f + backdropNotch * 0.62f);
        float backdropHeight = 210.0f + backdropBand * 135.0f + silhouetteRidge * 155.0f + backdropNoise * 30.0f;
        height = Lerp(height, Math.Max(height, backdropHeight), backdropInfluence * 0.58f);
        float westCorridor = Smooth(Clamp((192.0f - x - 520.0f) / 820.0f, 0.0f, 1.0f));
        float eastCorridor = Smooth(Clamp((x - 192.0f - 520.0f) / 820.0f, 0.0f, 1.0f));
        float southBlend = Smooth(Clamp((360.0f - z) / 1200.0f, 0.0f, 1.0f));
        float westNorthBlend = Smooth(Clamp((z - 360.0f) / 920.0f, 0.0f, 1.0f));
        float routeDistanceBand = Smooth(Clamp((originDistance - 780.0f) / 420.0f, 0.0f, 1.0f)) * (1.0f - Smooth(Clamp((originDistance - 4300.0f) / 1200.0f, 0.0f, 1.0f)));
        float routeCorridor = routeDistanceBand * Clamp(westCorridor * (0.50f + southBlend * 0.42f + westNorthBlend * 0.30f) + eastCorridor * southBlend, 0.0f, 1.0f);
        float routeRidgeNoiseA = ValueNoise2D(x * 0.0024f + 113.0f, z * 0.0024f - 89.0f, Seed + 251u);
        float routeRidgeNoiseB = ValueNoise2D(x * 0.0068f - 37.0f, z * 0.0068f + 151.0f, Seed + 263u);
        float routeBreakup = ValueNoise2D(x * 0.0110f - 211.0f, z * 0.0110f + 73.0f, Seed + 281u);
        float routeNotch = Smooth(Clamp((routeBreakup - 0.02f) / 0.60f, 0.0f, 1.0f));
        float routeRidge = Clamp(0.26f + (1.0f - Math.Abs(routeRidgeNoiseA)) * 0.58f + routeRidgeNoiseB * 0.16f, 0.0f, 1.0f);
        float routeBackdropHeight = 260.0f + routeDistanceBand * 95.0f + routeRidge * 210.0f;
        height = Lerp(height, Math.Max(height, routeBackdropHeight), routeCorridor * routeRidge * routeNotch * 0.62f);
        return Math.Max(-332.0f, Math.Min(664.0f, height));
    }

    static V3 Cross(V3 a, V3 b) { return new V3(a.Y*b.Z-a.Z*b.Y, a.Z*b.X-a.X*b.Z, a.X*b.Y-a.Y*b.X); }
    static V3 Normalize(V3 v) { double l = Math.Sqrt(v.X*v.X+v.Y*v.Y+v.Z*v.Z); return l <= 1e-9 ? new V3(0,0,1) : new V3(v.X/l,v.Y/l,v.Z/l); }
    static List<string> ParseCsvLine(string line) {
        List<string> values = new List<string>(); StringBuilder cur = new StringBuilder(); bool quoted = false;
        for (int i = 0; i < line.Length; ++i) { char ch = line[i]; if (ch == '"') { if (quoted && i + 1 < line.Length && line[i + 1] == '"') { cur.Append('"'); ++i; } else quoted = !quoted; } else if (ch == ',' && !quoted) { values.Add(cur.ToString()); cur.Length = 0; } else cur.Append(ch); }
        values.Add(cur.ToString()); return values;
    }
    static bool Near(Color c, int r, int g, int b, int tol) { return Math.Abs(c.R-r)+Math.Abs(c.G-g)+Math.Abs(c.B-b) <= tol*3; }
    static float Pct(long v, long total) { return total > 0 ? (float)(v * 100.0 / total) : 0.0f; }
    static float Median(List<float> values) { if (values.Count == 0) return 0.0f; values.Sort(); return values[values.Count / 2]; }
    static float ParseFloat(string s) { float v; return Single.TryParse(s, NumberStyles.Float, CultureInfo.InvariantCulture, out v) ? v : 0.0f; }
    static string Num(float v) { return v.ToString("0.###", CultureInfo.InvariantCulture); }
    static string Csv(string s) { return "\"" + (s ?? "").Replace("\"", "\"\"") + "\""; }
    static float Clamp(float v, float lo, float hi) { return Math.Max(lo, Math.Min(hi, v)); }
    static float Smooth(float v) { v = Clamp(v, 0.0f, 1.0f); return v * v * (3.0f - 2.0f * v); }
    static float Lerp(float a, float b, float t) { return a + (b - a) * Clamp(t, 0.0f, 1.0f); }
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
[CameraSurfaceExposureAudit]::Run($LogPath, $NormalFrame490, $OwnerFrame490, $ExactQualityCsv, $OutputDir, $StartFrame, $EndFrame)
Write-Host "Wrote $OutputDir"
