param(
    [string]$NormalFrame,
    [string]$OwnerFrame,
    [string]$MaterialFrame,
    [string]$LodFrame = "",
    [string]$OutputDir,
    [int]$Frame = 301,
    [int]$MaxRows = 256
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($NormalFrame) -or !(Test-Path $NormalFrame)) {
    throw "Normal frame not found: $NormalFrame"
}
if ([string]::IsNullOrWhiteSpace($OwnerFrame) -or !(Test-Path $OwnerFrame)) {
    throw "Owner debug frame not found: $OwnerFrame"
}
if ([string]::IsNullOrWhiteSpace($MaterialFrame) -or !(Test-Path $MaterialFrame)) {
    throw "Material debug frame not found: $MaterialFrame"
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

public static class WaterShorelineAudit
{
    struct Sample
    {
        public int X, Y, WaterX, WaterY;
        public Color Normal, Owner, Material, Lod;
        public int DistanceToWater;
        public string BoundaryKind;
    }

    public static void Run(string normalPath, string ownerPath, string materialPath, string lodPath, string outputDir, int frame, int maxRows)
    {
        Directory.CreateDirectory(outputDir);
        using (Bitmap normal = new Bitmap(normalPath))
        using (Bitmap owner = new Bitmap(ownerPath))
        using (Bitmap material = new Bitmap(materialPath))
        using (Bitmap lod = String.IsNullOrWhiteSpace(lodPath) ? null : new Bitmap(lodPath))
        {
            int w = normal.Width;
            int h = normal.Height;
            bool[,] water = new bool[w, h];
            int waterCount = 0;
            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    Color c = normal.GetPixel(x, y);
                    bool isWater =
                        c.B >= 120 &&
                        c.G >= 80 &&
                        c.R <= 95 &&
                        c.B - c.R >= 45 &&
                        c.B - c.G >= 8;
                    water[x, y] = isWater;
                    if (isWater) ++waterCount;
                }
            }

            List<Sample> samples = new List<Sample>();
            int step = Math.Max(2, Math.Min(w, h) / 240);
            for (int y = h / 4; y < h - h / 12; y += step) {
                for (int x = w / 24; x < w - w / 24; x += step) {
                    if (water[x, y]) continue;
                    int wx, wy, d;
                    if (!NearestWater(water, w, h, x, y, 18, out wx, out wy, out d)) continue;
                    Color c = normal.GetPixel(x, y);
                    bool terrainLike =
                        (c.G >= 70 && c.R >= 50 && c.B <= 140) ||
                        (Math.Abs(c.R - c.G) < 38 && Math.Abs(c.G - c.B) < 42 && c.R > 55 && c.R < 180);
                    bool darkHole = c.R < 55 && c.G < 70 && c.B < 90;
                    if (!terrainLike && !darkHole) continue;
                    samples.Add(new Sample {
                        X = x,
                        Y = y,
                        WaterX = wx,
                        WaterY = wy,
                        DistanceToWater = d,
                        Normal = c,
                        Owner = owner.GetPixel(x, y),
                        Material = material.GetPixel(x, y),
                        Lod = lod == null ? Color.Black : lod.GetPixel(x, y),
                        BoundaryKind = darkHole ? "dark_gap_near_water" : "terrain_boundary_near_water"
                    });
                }
            }

            samples = samples
                .OrderBy(s => s.DistanceToWater)
                .ThenBy(s => s.Y)
                .ThenBy(s => s.X)
                .Take(Math.Max(1, maxRows))
                .ToList();

            string pixelCsv = Path.Combine(outputDir, "water_shoreline_pixels.csv");
            using (StreamWriter writer = new StreamWriter(pixelCsv, false, Encoding.UTF8)) {
                writer.WriteLine("frame,pixelX,pixelY,nearestWaterX,nearestWaterY,distanceToWater,boundaryKind,normalR,normalG,normalB,ownerLayer,materialDebug,likelyCause");
                foreach (Sample s in samples) {
                    string ownerName = ClassifyOwner(s.Owner, s.Lod);
                    string materialName = ClassifyMaterial(s.Material);
                    writer.WriteLine(String.Join(",",
                        frame.ToString(CultureInfo.InvariantCulture),
                        s.X.ToString(CultureInfo.InvariantCulture),
                        s.Y.ToString(CultureInfo.InvariantCulture),
                        s.WaterX.ToString(CultureInfo.InvariantCulture),
                        s.WaterY.ToString(CultureInfo.InvariantCulture),
                        s.DistanceToWater.ToString(CultureInfo.InvariantCulture),
                        Csv(s.BoundaryKind),
                        s.Normal.R.ToString(CultureInfo.InvariantCulture),
                        s.Normal.G.ToString(CultureInfo.InvariantCulture),
                        s.Normal.B.ToString(CultureInfo.InvariantCulture),
                        Csv(ownerName),
                        Csv(materialName),
                        Csv(LikelyCause(s.BoundaryKind, ownerName, materialName))));
                }
            }

            string summaryCsv = Path.Combine(outputDir, "water_shoreline_summary.csv");
            using (StreamWriter writer = new StreamWriter(summaryCsv, false, Encoding.UTF8)) {
                writer.WriteLine("metric,value");
                writer.WriteLine("waterPixelCount," + waterCount.ToString(CultureInfo.InvariantCulture));
                writer.WriteLine("sampleCount," + samples.Count.ToString(CultureInfo.InvariantCulture));
                foreach (var g in samples.GroupBy(s => ClassifyOwner(s.Owner, s.Lod) + "|" + ClassifyMaterial(s.Material) + "|" + s.BoundaryKind)
                                         .OrderByDescending(g => g.Count())) {
                    writer.WriteLine(Csv("group:" + g.Key) + "," + g.Count().ToString(CultureInfo.InvariantCulture));
                }
            }

            string overlayPath = Path.Combine(outputDir, "water_shoreline_overlay.bmp");
            using (Bitmap overlay = new Bitmap(normal))
            using (Graphics g = Graphics.FromImage(overlay)) {
                foreach (Sample s in samples) {
                    Color color = s.BoundaryKind == "dark_gap_near_water" ? Color.Magenta : Color.Yellow;
                    using (Pen p = new Pen(color, 2.0f)) {
                        g.DrawRectangle(p, s.X - 3, s.Y - 3, 6, 6);
                    }
                }
                overlay.Save(overlayPath);
            }
        }
    }

    static bool NearestWater(bool[,] water, int w, int h, int x, int y, int radius, out int wx, out int wy, out int d)
    {
        wx = 0; wy = 0; d = 0;
        for (int r = 1; r <= radius; ++r) {
            int y0 = Math.Max(0, y - r);
            int y1 = Math.Min(h - 1, y + r);
            int x0 = Math.Max(0, x - r);
            int x1 = Math.Min(w - 1, x + r);
            for (int yy = y0; yy <= y1; ++yy) {
                for (int xx = x0; xx <= x1; ++xx) {
                    if (Math.Max(Math.Abs(xx - x), Math.Abs(yy - y)) != r) continue;
                    if (water[xx, yy]) {
                        wx = xx; wy = yy; d = r;
                        return true;
                    }
                }
            }
        }
        return false;
    }

    static string ClassifyOwner(Color c, Color lod)
    {
        if (c.R == 0 && c.G == 0 && c.B == 0) return "unknown";
        if (c.R > 220 && c.G > 200 && c.B < 90) return "exact_sparse_surface";
        if (c.R > 220 && c.G >= 80 && c.G <= 170 && c.B < 90) return "exact_sparse_surface_protected_band";
        if (c.R > 220 && c.G < 90 && c.B > 180) return "exact_sparse_surface_far_band";
        if (c.G > 180 && c.R < 90 && c.B < 120) return "raymarch_exact_layer";
        if (c.B > 170 && c.R < 100) return "far_svo_or_sky";
        if (c.G > 120 && c.B > 150 && c.R < 90) return "water";
        if (c.R > 180 && c.G < 120 && c.B < 90) return "miss_or_error";
        return "mixed_or_unclassified";
    }

    static string ClassifyMaterial(Color c)
    {
        if (c.B > 160 && c.R < 90) return "water";
        if (c.R > 190 && c.G > 150 && c.B < 90) return "sand";
        if (c.G > 150 && c.R < 100) return "dirt";
        if (Math.Abs(c.R - c.G) < 30 && Math.Abs(c.G - c.B) < 30 && c.R > 85) return "stone";
        if (c.R < 50 && c.G < 60 && c.B < 80) return "air_or_gap";
        return "mixed_or_unclassified";
    }

    static string LikelyCause(string kind, string owner, string material)
    {
        if (kind == "dark_gap_near_water") return "dark non-water gap adjacent to water; inspect owner/material for occlusion or missing water";
        if (owner.StartsWith("exact_sparse_surface") && material != "water") return "exact terrain owns shoreline boundary";
        if (owner == "mid_voxel" && material != "water") return "mid voxel owns shoreline boundary";
        if (owner == "water") return "water owns boundary; visual issue likely water material/shape";
        return "mixed shoreline ownership";
    }

    static string Csv(string value)
    {
        if (value == null) return "";
        if (value.Contains(",") || value.Contains("\"") || value.Contains("\n") || value.Contains("\r")) {
            return "\"" + value.Replace("\"", "\"\"") + "\"";
        }
        return value;
    }
}
"@

Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition $source
[WaterShorelineAudit]::Run($NormalFrame, $OwnerFrame, $MaterialFrame, $LodFrame, $OutputDir, $Frame, $MaxRows)
