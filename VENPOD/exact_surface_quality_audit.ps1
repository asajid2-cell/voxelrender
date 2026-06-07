param(
    [string]$InputCsv = "build\captures\near_surface_gap_audit_frame0490_20260520\near_gap_pixels.csv",
    [string]$NormalFrame = "build\captures\mid_far_ablation_fixed_frame0490_20260520\normal\normal\engine_frame_0490.bmp",
    [string]$OutputDir = "build\captures\exact_surface_quality_audit_frame0490_20260520",
    [int]$MaxRows = 128
)

$ErrorActionPreference = "Stop"

if (!(Test-Path $InputCsv)) { throw "Input CSV not found: $InputCsv" }
if (!(Test-Path $NormalFrame)) { throw "Normal frame not found: $NormalFrame" }

$source = @"
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text;

public static class ExactSurfaceQualityAudit
{
    const float SeaLevel = -48.0f;
    const uint Seed = 12345u;

    sealed class Row
    {
        public Dictionary<string,string> V = new Dictionary<string,string>(StringComparer.OrdinalIgnoreCase);
        public string this[string key] { get { return V.ContainsKey(key) ? V[key] : ""; } }
    }

    struct N3
    {
        public float X, Y, Z;
        public N3(float x, float y, float z) { X = x; Y = y; Z = z; }
    }

    public static void Run(string inputCsv, string normalFrame, string outputDir, int maxRows)
    {
        Directory.CreateDirectory(outputDir);
        List<Row> rows = ReadCsv(inputCsv)
            .Where(r => r["ownerLayer"] == "exact_sparse_surface" && r["geometryState"] == "cpu_shader_expected_solid")
            .Take(Math.Max(1, maxRows))
            .ToList();

        string pixelCsv = Path.Combine(outputDir, "exact_surface_quality_pixels.csv");
        string tableCsv = Path.Combine(outputDir, "diagnosis_table.csv");
        string overlayPath = Path.Combine(outputDir, "exact_surface_quality_overlay.bmp");

        using (StreamWriter w = new StreamWriter(pixelCsv, false, Encoding.UTF8))
        {
            w.WriteLine("frame,region,pixelX,pixelY,worldX,worldY,worldZ,distance,dominantExactMaterial,expectedMaterial,normalX,normalY,normalZ,slopeDegrees,faceType,faceOrientationEstimate,sourceBrickX,sourceBrickY,sourceBrickZ,generatedOrEdited,localAirCount3,localDirtCount3,localStoneCount3,localSandCount3,localWaterCount3,localSolidCount3,terrainHeight,depthBelowHeight,localRelief4,ndotl,ambient,fogAmountEstimate,waterlineBand,classificationLikelyCause");
            foreach (Row r in rows)
            {
                float wx = F(r["worldX"]);
                float wy = F(r["worldY"]);
                float wz = F(r["worldZ"]);
                float terrainHeight = HeightAt(wx, wz);
                N3 normal = TerrainNormal(wx, wz, 2.0f);
                float slope = (float)(Math.Acos(Clamp(normal.Y, -1.0f, 1.0f)) * 180.0 / Math.PI);
                string faceType = FaceType(normal.Y, terrainHeight - wy);
                string expectedMaterial = MaterialAt(terrainHeight, wy);
                string dominantMaterial = String.IsNullOrWhiteSpace(r["cpuTerrainMaterialAtHit"]) ? expectedMaterial : r["cpuTerrainMaterialAtHit"];
                int bx = (int)Math.Floor(wx / 16.0f);
                int by = (int)Math.Floor(wy / 16.0f);
                int bz = (int)Math.Floor(wz / 16.0f);
                Dictionary<string,int> counts = NeighborhoodCounts(wx, wy, wz, 1);
                float relief = Relief(wx, wz, 8.0f);
                float ndotl = Clamp(normal.X * 0.45f + normal.Y * 0.82f + normal.Z * 0.34f, 0.0f, 1.0f);
                float dist = F(r["estimatedDistance"]);
                float fog = Clamp((dist - 1500.0f) / 4500.0f, 0.0f, 1.0f) * 0.35f;
                float waterline = 1.0f - Clamp((wy - SeaLevel + 2.0f) / 14.0f, 0.0f, 1.0f);
                string cause = LikelyCause(dominantMaterial, faceType, terrainHeight - wy, relief, waterline, dist);

                w.WriteLine(String.Join(",",
                    Csv(r["frame"]),
                    Csv(r["region"]),
                    Csv(r["pixelX"]),
                    Csv(r["pixelY"]),
                    Num(wx),
                    Num(wy),
                    Num(wz),
                    Num(dist),
                    Csv(dominantMaterial),
                    Csv(expectedMaterial),
                    Num(normal.X),
                    Num(normal.Y),
                    Num(normal.Z),
                    Num(slope),
                    Csv(faceType),
                    Csv(Orientation(normal)),
                    bx.ToString(CultureInfo.InvariantCulture),
                    by.ToString(CultureInfo.InvariantCulture),
                    bz.ToString(CultureInfo.InvariantCulture),
                    Csv("generated"),
                    counts["air"].ToString(CultureInfo.InvariantCulture),
                    counts["dirt"].ToString(CultureInfo.InvariantCulture),
                    counts["stone"].ToString(CultureInfo.InvariantCulture),
                    counts["sand"].ToString(CultureInfo.InvariantCulture),
                    counts["water"].ToString(CultureInfo.InvariantCulture),
                    (counts["dirt"] + counts["stone"] + counts["sand"]).ToString(CultureInfo.InvariantCulture),
                    Num(terrainHeight),
                    Num(terrainHeight - wy),
                    Num(relief),
                    Num(ndotl),
                    Num(0.72f),
                    Num(fog),
                    Num(waterline),
                    Csv(cause)));
            }
        }

        WriteDiagnosis(rows, tableCsv);
        WriteOverlay(rows, normalFrame, overlayPath);
    }

    static void WriteDiagnosis(List<Row> rows, string tableCsv)
    {
        var groups = rows.GroupBy(r => r["region"]).OrderBy(g => g.Key);
        using (StreamWriter w = new StreamWriter(tableCsv, false, Encoding.UTF8))
        {
            w.WriteLine("region,samples,dominantExactMaterial,dominantFaceType,medianDistance,medianDepthBelowHeight,medianSlopeDegrees,generatedEdited,fog,likelyCause,evidenceArtifact");
            foreach (var g in groups)
            {
                List<float> distances = new List<float>();
                List<float> depths = new List<float>();
                List<float> slopes = new List<float>();
                Dictionary<string,int> mats = new Dictionary<string,int>();
                Dictionary<string,int> faces = new Dictionary<string,int>();
                foreach (Row r in g)
                {
                    float wx = F(r["worldX"]);
                    float wy = F(r["worldY"]);
                    float wz = F(r["worldZ"]);
                    float h = HeightAt(wx, wz);
                    N3 n = TerrainNormal(wx, wz, 2.0f);
                    string mat = String.IsNullOrWhiteSpace(r["cpuTerrainMaterialAtHit"]) ? MaterialAt(h, wy) : r["cpuTerrainMaterialAtHit"];
                    string face = FaceType(n.Y, h - wy);
                    distances.Add(F(r["estimatedDistance"]));
                    depths.Add(h - wy);
                    slopes.Add((float)(Math.Acos(Clamp(n.Y, -1.0f, 1.0f)) * 180.0 / Math.PI));
                    Bump(mats, mat);
                    Bump(faces, face);
                }
                string material = Top(mats);
                string faceType = Top(faces);
                float medianDistance = Median(distances);
                float medianDepth = Median(depths);
                float medianSlope = Median(slopes);
                string cause = RegionCause(material, faceType, medianDistance, medianDepth, medianSlope);
                w.WriteLine(String.Join(",",
                    Csv(g.Key),
                    g.Count().ToString(CultureInfo.InvariantCulture),
                    Csv(material),
                    Csv(faceType),
                    Num(medianDistance),
                    Num(medianDepth),
                    Num(medianSlope),
                    Csv("generated"),
                    Csv(medianDistance < 1500.0f ? "low_or_none" : "distance_fog"),
                    Csv(cause),
                    Csv("exact_surface_quality_pixels.csv")));
            }
        }
    }

    static void WriteOverlay(List<Row> rows, string normalFrame, string overlayPath)
    {
        using (Bitmap image = new Bitmap(normalFrame))
        using (Graphics g = Graphics.FromImage(image))
        {
            foreach (Row r in rows)
            {
                int x = I(r["pixelX"]);
                int y = I(r["pixelY"]);
                float wx = F(r["worldX"]);
                float wy = F(r["worldY"]);
                float wz = F(r["worldZ"]);
                float h = HeightAt(wx, wz);
                N3 n = TerrainNormal(wx, wz, 2.0f);
                string face = FaceType(n.Y, h - wy);
                Color c = face.Contains("top") ? Color.Lime :
                    face.Contains("waterline") ? Color.Cyan :
                    face.Contains("deep") ? Color.Magenta :
                    face.Contains("steep") ? Color.OrangeRed :
                    Color.Yellow;
                using (Pen p = new Pen(c, 2.0f))
                {
                    g.DrawRectangle(p, x - 4, y - 4, 8, 8);
                }
            }
            image.Save(overlayPath);
        }
    }

    static string LikelyCause(string material, string faceType, float depth, float relief, float waterline, float distance)
    {
        if (distance < 80.0f && depth > 8.0f) return "close exact sparse side/interior terrain face occluding view";
        if (faceType.Contains("deep")) return "exact surface exposes below-height or interior-like face";
        if (faceType.Contains("steep")) return "valid exact terrain, but steep side faces dominate readability";
        if (material == "stone" && waterline < 0.1f) return "material classification favors broad grey stone";
        if (waterline > 0.2f) return "shoreline exact terrain owns in front of water";
        if (relief > 28.0f) return "high-relief exact geometry reads jagged at voxel scale";
        return "exact surface material/lighting readability";
    }

    static string RegionCause(string material, string faceType, float distance, float depth, float slope)
    {
        if (distance < 80.0f && depth > 8.0f) return "near exact sparse terrain is present but close side/interior faces dominate the view";
        if (faceType.Contains("deep")) return "surface extraction or terrain shape exposes below-height faces";
        if (slope > 55.0f) return "exact geometry is mostly steep side faces, not missing coverage";
        if (material == "stone") return "material classification/stone shading makes valid exact terrain read grey";
        return "exact sparse surface readability issue";
    }

    static string FaceType(float ny, float depth)
    {
        if (depth > 14.0f) return "deep_below_height_side_or_interior";
        if (depth > 5.0f && ny < 0.86f) return "below_height_side_face";
        if (ny > 0.86f) return "top_surface";
        if (ny > 0.42f) return "steep_top_or_bank";
        return "steep_side_face";
    }

    static string Orientation(N3 n)
    {
        float ax = Math.Abs(n.X), ay = Math.Abs(n.Y), az = Math.Abs(n.Z);
        if (ay >= ax && ay >= az) return n.Y >= 0.0f ? "+Y/top" : "-Y/underside";
        if (ax >= az) return n.X >= 0.0f ? "+X/side" : "-X/side";
        return n.Z >= 0.0f ? "+Z/side" : "-Z/side";
    }

    static Dictionary<string,int> NeighborhoodCounts(float wx, float wy, float wz, int radius)
    {
        Dictionary<string,int> counts = new Dictionary<string,int> {
            {"air", 0}, {"dirt", 0}, {"stone", 0}, {"sand", 0}, {"water", 0}
        };
        int ix = (int)Math.Round(wx), iy = (int)Math.Round(wy), iz = (int)Math.Round(wz);
        for (int dz = -radius; dz <= radius; ++dz)
        for (int dy = -radius; dy <= radius; ++dy)
        for (int dx = -radius; dx <= radius; ++dx)
        {
            float h = HeightAt(ix + dx, iz + dz);
            string mat = (iy + dy) <= Math.Max(h, SeaLevel) ? MaterialAt(h, iy + dy) : "air";
            if (!counts.ContainsKey(mat)) counts[mat] = 0;
            counts[mat]++;
        }
        return counts;
    }

    static N3 TerrainNormal(float x, float z, float step)
    {
        float hx0 = HeightAt(x - step, z);
        float hx1 = HeightAt(x + step, z);
        float hz0 = HeightAt(x, z - step);
        float hz1 = HeightAt(x, z + step);
        N3 n = new N3(-(hx1 - hx0) / (2.0f * step), 1.0f, -(hz1 - hz0) / (2.0f * step));
        float len = (float)Math.Sqrt(n.X * n.X + n.Y * n.Y + n.Z * n.Z);
        return len <= 1e-6f ? new N3(0, 1, 0) : new N3(n.X / len, n.Y / len, n.Z / len);
    }

    static float Relief(float x, float z, float offset)
    {
        float center = HeightAt(x, z);
        float min = center, max = center;
        float[] xs = { x - offset, x, x + offset };
        float[] zs = { z - offset, z, z + offset };
        foreach (float sx in xs)
        foreach (float sz in zs)
        {
            float h = HeightAt(sx, sz);
            min = Math.Min(min, h);
            max = Math.Max(max, h);
        }
        return max - min;
    }

    static string MaterialAt(float height, double y)
    {
        if (y <= SeaLevel && height < SeaLevel) return "water";
        if (height < SeaLevel + 6.0f) return "sand";
        if (height > 160.0f) return "stone";
        return "dirt";
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
        height = Lerp(height, publicRegionHeight, originComfort * 0.94f);
        float publicCapInfluence = 1.0f - Smooth(Clamp((originDistance - 220.0f) / 420.0f, 0.0f, 1.0f));
        float publicCap = 58.0f + Smooth(Clamp(originDistance / 640.0f, 0.0f, 1.0f)) * 114.0f;
        height = Lerp(height, Math.Min(height, publicCap), publicCapInfluence);
        float submergedBlend = 1.0f - Smooth(Clamp((height - (SeaLevel + 28.0f)) / 86.0f, 0.0f, 1.0f));
        if (submergedBlend > 0.0f) {
            float shelf = (SeaLevel - 8.0f) + broad * 38.0f + ridgeHeight * 22.0f + detail * 2.0f + (1.0f - Smooth(Clamp(originDistance / 520.0f, 0.0f, 1.0f))) * 18.0f;
            height = Lerp(height, shelf, submergedBlend * 0.55f);
        }
        float playableBankBand = 1.0f - Smooth(Clamp((originDistance - 260.0f) / 980.0f, 0.0f, 1.0f));
        float lowlandUpper = 1.0f - Smooth(Clamp((height - (SeaLevel + 96.0f)) / 120.0f, 0.0f, 1.0f));
        float lowlandFloor = Smooth(Clamp((height - (SeaLevel - 40.0f)) / 64.0f, 0.0f, 1.0f));
        float playableBankBlend = playableBankBand * lowlandUpper * lowlandFloor * 0.64f;
        if (playableBankBlend > 0.0f) {
            float playableShelfHeight = (SeaLevel + 18.0f) + broad * 28.0f + ridgeHeight * 10.0f + detail * 1.5f + (1.0f - Smooth(Clamp(originDistance / 460.0f, 0.0f, 1.0f))) * 42.0f;
            height = Lerp(height, playableShelfHeight, playableBankBlend);
        }
        float publicBasinBand =
            Smooth(Clamp((originDistance - 360.0f) / 240.0f, 0.0f, 1.0f)) *
            (1.0f - Smooth(Clamp((originDistance - 1700.0f) / 760.0f, 0.0f, 1.0f))) *
            Smooth(Clamp((height - (SeaLevel - 38.0f)) / 56.0f, 0.0f, 1.0f)) *
            (1.0f - Smooth(Clamp((height - (SeaLevel + 180.0f)) / 140.0f, 0.0f, 1.0f)));
        if (publicBasinBand > 0.0f) {
            float publicBasinFloor = (SeaLevel - 12.0f) + broad * 2.0f + detail * 0.35f;
            height = Lerp(height, Math.Min(height, publicBasinFloor), publicBasinBand * 0.80f);
        }
        float backdropNoise = ValueNoise2D(x * 0.0018f + 19.0f, z * 0.0018f - 31.0f, Seed + 211u);
        float backdropRidgeSource = ValueNoise2D(x * 0.0032f - 71.0f, z * 0.0032f + 43.0f, Seed + 227u);
        float backdropRidge = 1.0f - Math.Abs(backdropRidgeSource);
        float backdropBreakup = ValueNoise2D(x * 0.0075f + 203.0f, z * 0.0075f - 167.0f, Seed + 271u);
        float backdropNotch = Smooth(Clamp((backdropBreakup - 0.08f) / 0.58f, 0.0f, 1.0f));
        float silhouetteRidge = Clamp(backdropRidge * backdropRidge * 1.22f + backdropNoise * 0.18f, 0.0f, 1.0f);
        float backdropBand = Smooth(Clamp((originDistance - 1360.0f) / 700.0f, 0.0f, 1.0f)) * (1.0f - Smooth(Clamp((originDistance - 5200.0f) / 1200.0f, 0.0f, 1.0f)));
        float northBackdrop = Smooth(Clamp((z - 1180.0f) / 900.0f, 0.0f, 1.0f));
        float sideBackdrop = Smooth(Clamp((Math.Abs(x - 192.0f) - 820.0f) / 980.0f, 0.0f, 1.0f));
        float backdropInfluence = backdropBand * Clamp(northBackdrop + sideBackdrop * 0.58f, 0.0f, 1.0f) * Smooth(silhouetteRidge) * (0.46f + backdropNotch * 0.54f);
        float backdropHeight = 248.0f + backdropBand * 160.0f + silhouetteRidge * 186.0f + backdropNoise * 26.0f;
        height = Lerp(height, Math.Max(height, backdropHeight), backdropInfluence * 0.70f);
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
        float routeBackdropHeight = 272.0f + routeDistanceBand * 104.0f + routeRidge * 218.0f;
        height = Lerp(height, Math.Max(height, routeBackdropHeight), routeCorridor * routeRidge * routeNotch * 0.68f);
        return Math.Max(-332.0f, Math.Min(664.0f, height));
    }

    static List<Row> ReadCsv(string path)
    {
        string[] lines = File.ReadAllLines(path);
        if (lines.Length == 0) return new List<Row>();
        List<string> headers = ParseCsvLine(lines[0]);
        List<Row> rows = new List<Row>();
        for (int i = 1; i < lines.Length; ++i)
        {
            if (String.IsNullOrWhiteSpace(lines[i])) continue;
            List<string> vals = ParseCsvLine(lines[i]);
            Row r = new Row();
            for (int h = 0; h < headers.Count; ++h) r.V[headers[h]] = h < vals.Count ? vals[h] : "";
            rows.Add(r);
        }
        return rows;
    }

    static List<string> ParseCsvLine(string line)
    {
        List<string> values = new List<string>();
        StringBuilder current = new StringBuilder();
        bool quoted = false;
        for (int i = 0; i < line.Length; ++i)
        {
            char ch = line[i];
            if (ch == '"') {
                if (quoted && i + 1 < line.Length && line[i + 1] == '"') { current.Append('"'); ++i; }
                else quoted = !quoted;
            } else if (ch == ',' && !quoted) {
                values.Add(current.ToString());
                current.Length = 0;
            } else {
                current.Append(ch);
            }
        }
        values.Add(current.ToString());
        return values;
    }

    static void Bump(Dictionary<string,int> d, string k) { if (!d.ContainsKey(k)) d[k] = 0; d[k]++; }
    static string Top(Dictionary<string,int> d) { return d.Count == 0 ? "" : d.OrderByDescending(kv => kv.Value).ThenBy(kv => kv.Key).First().Key; }
    static int I(string s) { int v; return Int32.TryParse(s, NumberStyles.Integer, CultureInfo.InvariantCulture, out v) ? v : 0; }
    static float F(string s) { float v; return Single.TryParse(s, NumberStyles.Float, CultureInfo.InvariantCulture, out v) ? v : 0.0f; }
    static string Num(float v) { return v.ToString("0.###", CultureInfo.InvariantCulture); }
    static string Csv(string s) { return "\"" + (s ?? "").Replace("\"", "\"\"") + "\""; }
    static float Median(List<float> values) { if (values.Count == 0) return 0.0f; values.Sort(); return values[values.Count / 2]; }
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
[ExactSurfaceQualityAudit]::Run($InputCsv, $NormalFrame, $OutputDir, $MaxRows)
Write-Host "Wrote $OutputDir"
