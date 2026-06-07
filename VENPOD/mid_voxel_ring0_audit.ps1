param(
    [string]$MidFarAuditCsv,
    [string]$LogPath,
    [string]$NormalFrame,
    [int]$Frame = 300,
    [string]$OutputCsv
)

$ErrorActionPreference = "Stop"
if (-not (Test-Path $MidFarAuditCsv)) { throw "Missing -MidFarAuditCsv: $MidFarAuditCsv" }
if (-not (Test-Path $LogPath)) { throw "Missing -LogPath: $LogPath" }
if (-not (Test-Path $NormalFrame)) { throw "Missing -NormalFrame: $NormalFrame" }
if ([string]::IsNullOrWhiteSpace($OutputCsv)) { throw "-OutputCsv is required" }

$source = @"
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Globalization;
using System.IO;
using System.Text.RegularExpressions;

public static class MidVoxelRing0Audit
{
    const float SeaLevel = -48.0f;
    const uint Seed = 12345u;
    struct V3 {
        public double X, Y, Z;
        public V3(double x, double y, double z) { X = x; Y = y; Z = z; }
        public static V3 operator +(V3 a, V3 b) { return new V3(a.X + b.X, a.Y + b.Y, a.Z + b.Z); }
        public static V3 operator *(V3 a, double s) { return new V3(a.X * s, a.Y * s, a.Z * s); }
    }
    struct Camera { public V3 Pos, Forward; public double Yaw, Pitch; }

    public static void Run(string inputCsv, string logPath, string normalPath, int frame, string outputCsv)
    {
        Camera camera = ParseCamera(logPath, frame);
        using (Bitmap normal = new Bitmap(normalPath))
        using (StreamWriter writer = new StreamWriter(outputCsv, false))
        {
            writer.WriteLine("frame,region,pixelX,pixelY,material,distance,cellSize,faceType,normalX,normalY,normalZ,worldX,worldY,worldZ,airNeighbors,solidNeighbors,generatedOccupancyClass,exactRangeState,likelyCause");
            string[] lines = File.ReadAllLines(inputCsv);
            if (lines.Length <= 1) return;
            string[] header = SplitCsv(lines[0]);
            Dictionary<string,int> ix = new Dictionary<string,int>();
            for (int i = 0; i < header.Length; ++i) ix[header[i]] = i;
            for (int i = 1; i < lines.Length; ++i)
            {
                string[] c = SplitCsv(lines[i]);
                if (Get(c, ix, "owner") != "mid_voxel") continue;
                if (Get(c, ix, "lodOrDepth") != "ring0") continue;
                if (Get(c, ix, "cellSize") != "12") continue;
                int x = Int(Get(c, ix, "pixelX"));
                int y = Int(Get(c, ix, "pixelY"));
                double t = D(Get(c, ix, "shaderHitDistance"));
                V3 ray = BuildRay(camera, normal.Width, normal.Height, x, y);
                V3 p = camera.Pos + ray * t;
                double cell = 12.0;
                int air = 0;
                air += IsGeneratedAir(new V3(p.X + cell, p.Y, p.Z)) ? 1 : 0;
                air += IsGeneratedAir(new V3(p.X - cell, p.Y, p.Z)) ? 1 : 0;
                air += IsGeneratedAir(new V3(p.X, p.Y + cell, p.Z)) ? 1 : 0;
                air += IsGeneratedAir(new V3(p.X, p.Y - cell, p.Z)) ? 1 : 0;
                air += IsGeneratedAir(new V3(p.X, p.Y, p.Z + cell)) ? 1 : 0;
                air += IsGeneratedAir(new V3(p.X, p.Y, p.Z - cell)) ? 1 : 0;
                int solid = 6 - air;
                string occ = air <= 1 ? "mostly_embedded" : (air <= 3 ? "terrain_boundary" : "thin_or_isolated");
                string face = Get(c, ix, "faceType");
                string material = Get(c, ix, "material");
                string exact = t <= 2048.0 ? "within_surface_raster_range" : "beyond_surface_raster_range";
                string likely = "ring0_cell12_shading_or_grid";
                if (face == "side_face") likely = "ring0_cell12_exposed_side_face";
                if (occ == "thin_or_isolated") likely = "thin_generated_mid_voxel_feature";
                if (exact == "within_surface_raster_range" && face == "side_face") likely = "exact_surface_not_covering_close_mid_side_face";
                writer.WriteLine(String.Join(",",
                    frame.ToString(CultureInfo.InvariantCulture),
                    Q(Get(c, ix, "region")),
                    x.ToString(CultureInfo.InvariantCulture),
                    y.ToString(CultureInfo.InvariantCulture),
                    Q(material),
                    F(t),
                    "12",
                    Q(face),
                    Get(c, ix, "normalX"),
                    Get(c, ix, "normalY"),
                    Get(c, ix, "normalZ"),
                    F(p.X), F(p.Y), F(p.Z),
                    air.ToString(CultureInfo.InvariantCulture),
                    solid.ToString(CultureInfo.InvariantCulture),
                    Q(occ),
                    Q(exact),
                    Q(likely)));
            }
        }
    }

    static bool IsGeneratedAir(V3 p) {
        float h = HeightAt((float)p.X, (float)p.Z);
        float surface = h < SeaLevel ? SeaLevel : h;
        return p.Y > surface;
    }

    static Camera ParseCamera(string logPath, int frame)
    {
        string text = File.ReadAllText(logPath);
        Regex re = new Regex(@"PERF_(?:SPARSE_WALK|CAMERA_EXPOSURE) frame=" + frame + @".*cam=\(([-0-9.]+),([-0-9.]+),([-0-9.]+)\).*yaw=([-0-9.]+) pitch=([-0-9.]+) forward=\(([-0-9.]+),([-0-9.]+),([-0-9.]+)\)");
        Match m = re.Match(text);
        if (!m.Success) throw new InvalidOperationException("Could not find camera exposure for frame " + frame);
        return new Camera {
            Pos = new V3(D(m.Groups[1].Value), D(m.Groups[2].Value), D(m.Groups[3].Value)),
            Yaw = D(m.Groups[4].Value),
            Pitch = D(m.Groups[5].Value),
            Forward = Normalize(new V3(D(m.Groups[6].Value), D(m.Groups[7].Value), D(m.Groups[8].Value)))
        };
    }

    static V3 BuildRay(Camera c, int width, int height, int px, int py)
    {
        double fov = 60.0 * Math.PI / 180.0;
        double aspect = (double)width / Math.Max(1, height);
        double ndcX = (((px + 0.5) / width) * 2.0 - 1.0);
        double ndcY = -(((py + 0.5) / height) * 2.0 - 1.0);
        V3 forward = c.Forward;
        V3 right = Normalize(Cross(forward, new V3(0, 1, 0)));
        V3 up = Normalize(Cross(right, forward));
        double tanHalf = Math.Tan(fov * 0.5);
        return Normalize(new V3(
            forward.X + right.X * ndcX * tanHalf * aspect + up.X * ndcY * tanHalf,
            forward.Y + right.Y * ndcX * tanHalf * aspect + up.Y * ndcY * tanHalf,
            forward.Z + right.Z * ndcX * tanHalf * aspect + up.Z * ndcY * tanHalf));
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
        height = Lerp(height, publicRegionHeight, originComfort * 0.82f);
        float publicCapInfluence = 1.0f - Smooth(Clamp((originDistance - 220.0f) / 420.0f));
        float publicCap = 76.0f + Smooth(Clamp(originDistance / 640.0f)) * 96.0f;
        height = Lerp(height, Math.Min(height, publicCap), publicCapInfluence);
        float submergedBlend = 1.0f - Smooth(Clamp((height - (SeaLevel + 28.0f)) / 86.0f));
        if (submergedBlend > 0.0f) {
            float shelf = (SeaLevel - 8.0f) + broad * 38.0f + ridgeHeight * 22.0f + detail * 2.0f + (1.0f - Smooth(originDistance / 520.0f)) * 18.0f;
            height = Lerp(height, shelf, submergedBlend * 0.55f);
        }
        return Math.Max(-332.0f, Math.Min(664.0f, height));
    }

    static string[] SplitCsv(string line) {
        List<string> parts = new List<string>();
        bool q = false; string cur = "";
        for (int i = 0; i < line.Length; ++i) {
            char ch = line[i];
            if (ch == '\"') { q = !q; continue; }
            if (ch == ',' && !q) { parts.Add(cur); cur = ""; } else { cur += ch; }
        }
        parts.Add(cur);
        return parts.ToArray();
    }
    static string Get(string[] c, Dictionary<string,int> ix, string name) { return ix.ContainsKey(name) && ix[name] < c.Length ? c[ix[name]] : ""; }
    static int Int(string s) { int v = 0; int.TryParse(s, NumberStyles.Any, CultureInfo.InvariantCulture, out v); return v; }
    static double D(string s) { double v = 0.0; double.TryParse(s, NumberStyles.Any, CultureInfo.InvariantCulture, out v); return v; }
    static string F(double v) { return v.ToString("0.###", CultureInfo.InvariantCulture); }
    static string Q(string s) { return "\"" + s.Replace("\"", "\"\"") + "\""; }
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
$parent = Split-Path -Parent $OutputCsv
if ($parent) { New-Item -ItemType Directory -Force -Path $parent | Out-Null }
[MidVoxelRing0Audit]::Run($MidFarAuditCsv, $LogPath, $NormalFrame, $Frame, $OutputCsv)
Write-Host "Wrote $OutputCsv"
