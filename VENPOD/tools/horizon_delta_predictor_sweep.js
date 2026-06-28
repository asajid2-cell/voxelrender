#!/usr/bin/env node
"use strict";

const fs = require("fs");

function usage() {
  console.error([
    "usage: node tools/horizon_delta_predictor_sweep.js --reference <bmp> --candidate <bmp> [options]",
    "",
    "Ranks full-res horizon tiles using low-res-only features and scores against the",
    "full-vs-low-res visual oracle. The reference image is used only for scoring.",
    "",
    "Options:",
    "  --tile-size <n>           Full-res tile size in pixels (default 4)",
    "  --visual-threshold <n>    Oracle per-channel delta threshold (default 96)",
    "  --y0 <n>                  Horizon candidate band start row (default 320)",
    "  --y1 <n>                  Horizon candidate band end row (default 480)",
    "  --oracle-y0 <n>           Oracle start row; default is full-frame",
    "  --oracle-y1 <n>           Oracle end row; default is full-frame",
    "  --bg-scale <n>            Virtual low-res background scale (default 0.25)",
    "  --lowres-source <bmp>     Use this actual pre-composite low-res BMP for features",
    "  --bg-width <n>            Override virtual low-res width",
    "  --bg-height <n>           Override virtual low-res height",
    "  --lowres <average|center> Virtual low-res reconstruction mode (default average)",
    "  --budgets <csv>           Tile budgets to report (default 80,160,320,640,1280,2560)",
    "  --require-cover-budget <n> Exit nonzero unless some feature covers oracle within N tiles",
    "  --require-feature <name>   With --require-cover-budget, require this specific feature",
    "  --lowres-base-ms <n>       Cost model low-res base ms",
    "  --per-tile-ms <n>          Cost model repair ms per selected tile",
    "  --target-ms <n>            Cost model target ms; checked at full oracle coverage",
    "  --score-threshold <n>      Fixed score threshold to evaluate for each feature",
    "  --require-score-threshold  Gate on the fixed threshold instead of the rank oracle",
    "  --json                    Emit JSON only",
  ].join("\n"));
}

function parseArgs(argv) {
  const args = {
    tileSize: 4,
    visualThreshold: 96,
    y0: 320,
    y1: 480,
    bgScale: 0.25,
    lowres: "average",
    budgets: [80, 160, 320, 640, 1280, 2560],
    requireCoverBudget: null,
    requireScoreThreshold: false,
    json: false,
  };
  for (let i = 2; i < argv.length; ++i) {
    const key = argv[i];
    const value = argv[i + 1];
    switch (key) {
    case "--reference":
      args.reference = value;
      ++i;
      break;
    case "--candidate":
      args.candidate = value;
      ++i;
      break;
    case "--lowres-source":
      args.lowresSource = value;
      ++i;
      break;
    case "--tile-size":
      args.tileSize = Number(value);
      ++i;
      break;
    case "--visual-threshold":
      args.visualThreshold = Number(value);
      ++i;
      break;
    case "--y0":
      args.y0 = Number(value);
      ++i;
      break;
    case "--y1":
      args.y1 = Number(value);
      ++i;
      break;
    case "--oracle-y0":
      args.oracleY0 = Number(value);
      ++i;
      break;
    case "--oracle-y1":
      args.oracleY1 = Number(value);
      ++i;
      break;
    case "--bg-scale":
      args.bgScale = Number(value);
      ++i;
      break;
    case "--bg-width":
      args.bgWidth = Number(value);
      ++i;
      break;
    case "--bg-height":
      args.bgHeight = Number(value);
      ++i;
      break;
    case "--lowres":
      args.lowres = value;
      ++i;
      break;
    case "--budgets":
      args.budgets = value.split(",").map((v) => Number(v.trim())).filter((v) => Number.isFinite(v) && v > 0);
      ++i;
      break;
    case "--require-cover-budget":
      args.requireCoverBudget = Number(value);
      ++i;
      break;
    case "--require-feature":
      args.requireFeature = value;
      ++i;
      break;
    case "--lowres-base-ms":
      args.lowresBaseMs = Number(value);
      ++i;
      break;
    case "--per-tile-ms":
      args.perTileMs = Number(value);
      ++i;
      break;
    case "--target-ms":
      args.targetMs = Number(value);
      ++i;
      break;
    case "--score-threshold":
      args.scoreThreshold = Number(value);
      ++i;
      break;
    case "--require-score-threshold":
      args.requireScoreThreshold = true;
      break;
    case "--json":
      args.json = true;
      break;
    case "--help":
    case "-h":
      usage();
      process.exit(0);
      break;
    default:
      throw new Error(`unknown argument: ${key}`);
    }
  }
  if (!args.reference || !args.candidate ||
      !Number.isFinite(args.tileSize) || args.tileSize <= 0 ||
      !Number.isFinite(args.visualThreshold) ||
      !Number.isFinite(args.y0) || !Number.isFinite(args.y1) ||
      (args.oracleY0 !== undefined && !Number.isFinite(args.oracleY0)) ||
      (args.oracleY1 !== undefined && !Number.isFinite(args.oracleY1)) ||
      !Number.isFinite(args.bgScale) || args.bgScale <= 0 ||
      !["average", "center"].includes(args.lowres) ||
      args.budgets.length === 0 ||
      (args.requireCoverBudget !== null && !Number.isFinite(args.requireCoverBudget))) {
    usage();
    process.exit(2);
  }
  for (const field of ["lowresBaseMs", "perTileMs", "targetMs"]) {
    if (args[field] !== undefined && (!Number.isFinite(args[field]) || args[field] < 0)) {
      usage();
      process.exit(2);
    }
  }
  if (args.scoreThreshold !== undefined &&
      (!Number.isFinite(args.scoreThreshold) || args.scoreThreshold < 0)) {
    usage();
    process.exit(2);
  }
  if (args.requireScoreThreshold && args.scoreThreshold === undefined) {
    usage();
    process.exit(2);
  }
  args.budgets.sort((a, b) => a - b);
  return args;
}

function readBmp24(filePath) {
  const data = fs.readFileSync(filePath);
  if (data.length < 54 || data.toString("ascii", 0, 2) !== "BM") {
    throw new Error(`${filePath}: not a BMP file`);
  }
  const offBits = data.readUInt32LE(10);
  const width = data.readInt32LE(18);
  const heightRaw = data.readInt32LE(22);
  const bpp = data.readUInt16LE(28);
  const compression = data.readUInt32LE(30);
  if (width <= 0 || heightRaw === 0 || bpp !== 24 || compression !== 0) {
    throw new Error(`${filePath}: expected uncompressed 24-bit BMP`);
  }
  const height = Math.abs(heightRaw);
  const topDown = heightRaw < 0;
  const stride = Math.floor((width * 3 + 3) / 4) * 4;
  return { filePath, data, offBits, width, height, topDown, stride };
}

function pixelOffset(img, x, yTop) {
  const yStorage = img.topDown ? yTop : img.height - 1 - yTop;
  return img.offBits + yStorage * img.stride + x * 3;
}

function readRgb255(img, x, y) {
  const o = pixelOffset(img, x, y);
  return [img.data[o + 2], img.data[o + 1], img.data[o + 0]];
}

function luma(rgb01) {
  return rgb01[0] * 0.2126 + rgb01[1] * 0.7152 + rgb01[2] * 0.0722;
}

function tileKey(tx, ty) {
  return `${tx},${ty}`;
}

function maxChannelDelta(ref, cand, x, y) {
  const ro = pixelOffset(ref, x, y);
  const co = pixelOffset(cand, x, y);
  return Math.max(
    Math.abs(ref.data[ro + 0] - cand.data[co + 0]),
    Math.abs(ref.data[ro + 1] - cand.data[co + 1]),
    Math.abs(ref.data[ro + 2] - cand.data[co + 2]));
}

function buildVirtualLowres(img, args) {
  const width = args.bgWidth || Math.max(1, Math.round(img.width * args.bgScale));
  const height = args.bgHeight || Math.max(1, Math.round(img.height * args.bgScale));
  const pixels = new Float32Array(width * height * 3);
  for (let by = 0; by < height; ++by) {
    const fy0 = Math.floor((by * img.height) / height);
    const fy1 = Math.max(fy0 + 1, Math.floor(((by + 1) * img.height) / height));
    for (let bx = 0; bx < width; ++bx) {
      const fx0 = Math.floor((bx * img.width) / width);
      const fx1 = Math.max(fx0 + 1, Math.floor(((bx + 1) * img.width) / width));
      let r = 0;
      let g = 0;
      let b = 0;
      let n = 0;
      if (args.lowres === "center") {
        const cx = Math.min(img.width - 1, Math.floor(((bx + 0.5) * img.width) / width));
        const cy = Math.min(img.height - 1, Math.floor(((by + 0.5) * img.height) / height));
        [r, g, b] = readRgb255(img, cx, cy);
        n = 1;
      } else {
        for (let y = fy0; y < Math.min(img.height, fy1); ++y) {
          for (let x = fx0; x < Math.min(img.width, fx1); ++x) {
            const rgb = readRgb255(img, x, y);
            r += rgb[0];
            g += rgb[1];
            b += rgb[2];
            ++n;
          }
        }
      }
      const o = (by * width + bx) * 3;
      pixels[o + 0] = (r / Math.max(1, n)) / 255.0;
      pixels[o + 1] = (g / Math.max(1, n)) / 255.0;
      pixels[o + 2] = (b / Math.max(1, n)) / 255.0;
    }
  }
  return { width, height, pixels };
}

function buildDirectLowres(img) {
  const pixels = new Float32Array(img.width * img.height * 3);
  for (let y = 0; y < img.height; ++y) {
    for (let x = 0; x < img.width; ++x) {
      const rgb = readRgb255(img, x, y);
      const o = (y * img.width + x) * 3;
      pixels[o + 0] = rgb[0] / 255.0;
      pixels[o + 1] = rgb[1] / 255.0;
      pixels[o + 2] = rgb[2] / 255.0;
    }
  }
  return { width: img.width, height: img.height, pixels };
}

function lowresRgb(bg, x, y) {
  const cx = Math.max(0, Math.min(bg.width - 1, x));
  const cy = Math.max(0, Math.min(bg.height - 1, y));
  const o = (cy * bg.width + cx) * 3;
  return [bg.pixels[o + 0], bg.pixels[o + 1], bg.pixels[o + 2]];
}

function selectOracle(ref, cand, tileSize, threshold, oracleY0, oracleY1) {
  const tileMaxDelta = new Map();
  const oracleTiles = new Set();
  let selectedPixels = 0;
  const yStart = Math.max(0, Math.min(ref.height, Math.floor(oracleY0 ?? 0)));
  const yEnd = Math.max(yStart, Math.min(ref.height, Math.ceil(oracleY1 ?? ref.height)));
  for (let y = yStart; y < yEnd; ++y) {
    for (let x = 0; x < ref.width; ++x) {
      const key = tileKey(Math.floor(x / tileSize), Math.floor(y / tileSize));
      const delta = maxChannelDelta(ref, cand, x, y);
      tileMaxDelta.set(key, Math.max(tileMaxDelta.get(key) || 0, delta));
      if (delta > threshold) {
        oracleTiles.add(key);
        ++selectedPixels;
      }
    }
  }
  return { oracleTiles, selectedPixels, tileMaxDelta, y0: yStart, y1: yEnd };
}

function computeTileFeatures(bg, fullWidth, fullHeight, tileSize, y0, y1) {
  const tilesX = Math.ceil(Math.max(1, fullWidth) / tileSize);
  const tilesY = Math.ceil(Math.max(1, fullHeight) / tileSize);
  const bandY0 = Math.min(y0, Math.max(1, fullHeight));
  const bandY1 = Math.min(Math.max(y1, bandY0), Math.max(1, fullHeight));
  const out = [];

  for (let ty = 0; ty < tilesY; ++ty) {
    const tileY0 = ty * tileSize;
    const tileY1 = Math.min(Math.max(1, fullHeight), tileY0 + tileSize);
    if (tileY1 <= bandY0 || tileY0 >= bandY1) {
      continue;
    }
    for (let tx = 0; tx < tilesX; ++tx) {
      const tileX0 = tx * tileSize;
      const tileX1 = Math.min(Math.max(1, fullWidth), tileX0 + tileSize);
      const bx0 = Math.floor((tileX0 * bg.width) / Math.max(1, fullWidth));
      const bx1 = Math.min(
        bg.width - 1,
        Math.max(
          bx0,
          Math.floor(((tileX1 * bg.width) + Math.max(1, fullWidth) - 1) / Math.max(1, fullWidth))));
      const by0 = Math.floor((tileY0 * bg.height) / Math.max(1, fullHeight));
      const by1 = Math.min(
        bg.height - 1,
        Math.max(
          by0,
          Math.floor(((tileY1 * bg.height) + Math.max(1, fullHeight) - 1) / Math.max(1, fullHeight))));

      const sx0 = Math.max(0, bx0 - 1);
      const sy0 = Math.max(0, by0 - 1);
      const sx1 = Math.min(bg.width - 1, bx1 + 1);
      const sy1 = Math.min(bg.height - 1, by1 + 1);

      let minLum = 1.0e9;
      let maxLum = -1.0e9;
      let sumLum = 0;
      let sumLum2 = 0;
      let samples = 0;
      const minRgb = [1.0e9, 1.0e9, 1.0e9];
      const maxRgb = [-1.0e9, -1.0e9, -1.0e9];
      let maxDx = 0;
      let maxDy = 0;
      let maxDyy = 0;

      for (let y = sy0; y <= sy1; ++y) {
        for (let x = sx0; x <= sx1; ++x) {
          const rgb = lowresRgb(bg, x, y);
          const lum = luma(rgb);
          minLum = Math.min(minLum, lum);
          maxLum = Math.max(maxLum, lum);
          sumLum += lum;
          sumLum2 += lum * lum;
          ++samples;
          for (let k = 0; k < 3; ++k) {
            minRgb[k] = Math.min(minRgb[k], rgb[k]);
            maxRgb[k] = Math.max(maxRgb[k], rgb[k]);
          }
          const lumL = luma(lowresRgb(bg, x - 1, y));
          const lumR = luma(lowresRgb(bg, x + 1, y));
          const lumU = luma(lowresRgb(bg, x, y - 1));
          const lumD = luma(lowresRgb(bg, x, y + 1));
          maxDx = Math.max(maxDx, Math.abs(lumR - lumL));
          maxDy = Math.max(maxDy, Math.abs(lumD - lumU));
          maxDyy = Math.max(maxDyy, Math.abs(lumU - 2.0 * lum + lumD));
        }
      }

      const lumaRange = maxLum - minLum;
      const rgbRange = Math.max(maxRgb[0] - minRgb[0], maxRgb[1] - minRgb[1], maxRgb[2] - minRgb[2]);
      const edge = Math.max(lumaRange, rgbRange);
      const meanLum = sumLum / Math.max(1, samples);
      const lumVariance = Math.max(0, (sumLum2 / Math.max(1, samples)) - meanLum * meanLum);
      const chromaRange = Math.max(
        Math.abs((maxRgb[0] - maxRgb[1]) - (minRgb[0] - minRgb[1])),
        Math.abs((maxRgb[2] - maxRgb[1]) - (minRgb[2] - minRgb[1])));
      const yCenter = (tileY0 + tileY1) * 0.5;
      const bandCenter = (bandY0 + bandY1) * 0.5;
      const halfBand = Math.max(1, (bandY1 - bandY0) * 0.5);
      const yBandPrior = Math.max(0, 1.0 - Math.abs(yCenter - bandCenter) / halfBand);
      out.push({
        key: tileKey(tx, ty),
        tx,
        ty,
        scores: {
          edge,
          lumaRange,
          rgbRange,
          verticalEdge: maxDy,
          horizontalEdge: maxDx,
          verticalDominance: Math.max(0, maxDy - maxDx),
          curvatureY: maxDyy,
          lumaVariance: lumVariance,
          chromaRange,
          edgeTimesVertical: edge * maxDy,
          edgeTimesCurvature: edge * maxDyy,
          varianceTimesEdge: lumVariance * edge,
          varianceTimesVertical: lumVariance * maxDy,
          varianceTimesCurvature: lumVariance * maxDyy,
          edgeTimesYBand: edge * yBandPrior,
          verticalTimesYBand: maxDy * yBandPrior,
        },
      });
    }
  }
  return { tiles: out, tilesX, tilesY, bandTiles: out.length };
}

function addCompositeFeatures(tileFeatures) {
  const baseNames = Object.keys(tileFeatures.tiles[0]?.scores || {});
  const ranges = new Map();
  for (const name of baseNames) {
    let minValue = Number.POSITIVE_INFINITY;
    let maxValue = Number.NEGATIVE_INFINITY;
    for (const tile of tileFeatures.tiles) {
      const value = tile.scores[name] || 0;
      minValue = Math.min(minValue, value);
      maxValue = Math.max(maxValue, value);
    }
    ranges.set(name, { minValue, maxValue });
  }
  const n = (tile, name) => {
    const range = ranges.get(name);
    if (!range || range.maxValue <= range.minValue) {
      return 0;
    }
    return ((tile.scores[name] || 0) - range.minValue) / (range.maxValue - range.minValue);
  };
  for (const tile of tileFeatures.tiles) {
    const edge = n(tile, "edge");
    const variance = n(tile, "lumaVariance");
    const vertical = n(tile, "verticalEdge");
    const dominance = n(tile, "verticalDominance");
    const curvature = n(tile, "curvatureY");
    const chroma = n(tile, "chromaRange");
    const yEdge = n(tile, "edgeTimesYBand");
    tile.scores.normVariancePlusEdge = variance + edge;
    tile.scores.normVarianceTimesEdge = variance * edge;
    tile.scores.normVariancePlusVertical = variance + vertical;
    tile.scores.normVariancePlusDominance = variance + dominance;
    tile.scores.normEdgeVarianceCurvature = edge + variance + curvature;
    tile.scores.normEdgeVarianceChroma = edge + variance + chroma;
    tile.scores.normYBandEdgeVariance = yEdge + variance;
  }
}

function rankAndScore(featureName, tileFeatures, oracle, args, screenPixels) {
  const ranked = [...tileFeatures.tiles].sort((a, b) => {
    const d = (b.scores[featureName] || 0) - (a.scores[featureName] || 0);
    if (d !== 0) {
      return d;
    }
    return a.key < b.key ? -1 : (a.key > b.key ? 1 : 0);
  });
  const rankByKey = new Map();
  ranked.forEach((tile, i) => rankByKey.set(tile.key, i + 1));

  let minTilesForFullOracle = 0;
  let cutoffScoreForFullOracle = null;
  const missingOracleInBand = [];
  for (const key of oracle.oracleTiles) {
    const rank = rankByKey.get(key);
    if (!rank) {
      missingOracleInBand.push(key);
    } else {
      minTilesForFullOracle = Math.max(minTilesForFullOracle, rank);
    }
  }
  if (missingOracleInBand.length > 0) {
    minTilesForFullOracle = null;
  } else if (minTilesForFullOracle > 0) {
    cutoffScoreForFullOracle = ranked[minTilesForFullOracle - 1]?.scores[featureName] ?? null;
  }

  const budgets = args.budgets.map((budget) => {
    const count = Math.min(budget, ranked.length);
    const selected = new Set(ranked.slice(0, count).map((tile) => tile.key));
    let missed = 0;
    for (const key of oracle.oracleTiles) {
      if (!selected.has(key)) {
        ++missed;
      }
    }
    let repairedMaxDelta = 0;
    for (const [key, delta] of oracle.tileMaxDelta) {
      if (!selected.has(key)) {
        repairedMaxDelta = Math.max(repairedMaxDelta, delta);
      }
    }
    return {
      budget,
      selectedTileCount: count,
      selectedPixelUpperPct:
        Math.round((1000000 * count * args.tileSize * args.tileSize) / screenPixels) / 10000,
      oracleMissed: missed,
      oracleCovered: oracle.oracleTiles.size - missed,
      repairedMaxDelta,
    };
  });
  let thresholdSelection = null;
  if (args.scoreThreshold !== undefined) {
    const selected = new Set();
    for (const tile of tileFeatures.tiles) {
      if ((tile.scores[featureName] || 0) >= args.scoreThreshold) {
        selected.add(tile.key);
      }
    }
    let missed = 0;
    for (const key of oracle.oracleTiles) {
      if (!selected.has(key)) {
        ++missed;
      }
    }
    let repairedMaxDelta = 0;
    for (const [key, delta] of oracle.tileMaxDelta) {
      if (!selected.has(key)) {
        repairedMaxDelta = Math.max(repairedMaxDelta, delta);
      }
    }
    const estimatedMs =
      args.lowresBaseMs !== undefined && args.perTileMs !== undefined
        ? args.lowresBaseMs + selected.size * args.perTileMs
        : null;
    thresholdSelection = {
      scoreThreshold: args.scoreThreshold,
      selectedTileCount: selected.size,
      selectedPixelUpperPct:
        Math.round((1000000 * selected.size * args.tileSize * args.tileSize) / screenPixels) / 10000,
      oracleMissed: missed,
      oracleCovered: oracle.oracleTiles.size - missed,
      repairedMaxDelta,
      estimatedMs,
      targetMarginMs:
        estimatedMs !== null && args.targetMs !== undefined ? args.targetMs - estimatedMs : null,
    };
  }

  const estimatedMsAtFullOracle =
    minTilesForFullOracle !== null &&
    args.lowresBaseMs !== undefined &&
    args.perTileMs !== undefined
      ? args.lowresBaseMs + minTilesForFullOracle * args.perTileMs
      : null;
  const targetMarginMs =
    estimatedMsAtFullOracle !== null && args.targetMs !== undefined
      ? args.targetMs - estimatedMsAtFullOracle
      : null;
  return {
    feature: featureName,
    minTilesForFullOracle,
    cutoffScoreForFullOracle,
    estimatedMsAtFullOracle,
    targetMarginMs,
    thresholdSelection,
    falsePositiveTilesAtFullOracle:
      minTilesForFullOracle === null ? null : Math.max(0, minTilesForFullOracle - oracle.oracleTiles.size),
    missingOracleInBand: missingOracleInBand.slice(0, 32),
    budgets,
  };
}

function main() {
  const args = parseArgs(process.argv);
  const ref = readBmp24(args.reference);
  const cand = readBmp24(args.candidate);
  if (ref.width !== cand.width || ref.height !== cand.height) {
    throw new Error(`dimension mismatch: reference ${ref.width}x${ref.height}, candidate ${cand.width}x${cand.height}`);
  }
  const lowresImg = args.lowresSource ? readBmp24(args.lowresSource) : null;
  const bg = lowresImg ? buildDirectLowres(lowresImg) : buildVirtualLowres(cand, args);
  const oracle = selectOracle(
    ref,
    cand,
    args.tileSize,
    args.visualThreshold,
    args.oracleY0,
    args.oracleY1);
  const tileFeatures = computeTileFeatures(bg, ref.width, ref.height, args.tileSize, args.y0, args.y1);
  addCompositeFeatures(tileFeatures);
  const featureNames = Object.keys(tileFeatures.tiles[0]?.scores || {});
  const screenPixels = ref.width * ref.height;
  const results = featureNames
    .map((name) => rankAndScore(name, tileFeatures, oracle, args, screenPixels))
    .sort((a, b) => {
      const av = a.minTilesForFullOracle === null ? Number.POSITIVE_INFINITY : a.minTilesForFullOracle;
      const bv = b.minTilesForFullOracle === null ? Number.POSITIVE_INFINITY : b.minTilesForFullOracle;
      return av - bv;
    });
  const best = results[0] || null;
  const output = {
    reference: args.reference,
    candidate: args.candidate,
    lowresSource: args.lowresSource || null,
    dimensions: { width: ref.width, height: ref.height },
    tileSize: args.tileSize,
    visualThreshold: args.visualThreshold,
    horizonBand: { y0: args.y0, y1: args.y1 },
    oracleBand: { y0: oracle.y0, y1: oracle.y1 },
    lowres: { mode: lowresImg ? "direct" : args.lowres, width: bg.width, height: bg.height },
    oracle: {
      selectedPixels: oracle.selectedPixels,
      tileCount: oracle.oracleTiles.size,
      repairedPixelUpperBound: oracle.oracleTiles.size * args.tileSize * args.tileSize,
      repairedPctUpperBound:
        Math.round((1000000 * oracle.oracleTiles.size * args.tileSize * args.tileSize) / screenPixels) / 10000,
    },
    candidateTilesInBand: tileFeatures.bandTiles,
    budgets: args.budgets,
    costModel: {
      lowresBaseMs: args.lowresBaseMs ?? null,
      perTileMs: args.perTileMs ?? null,
      targetMs: args.targetMs ?? null,
    },
    best,
    results,
    notes: [
      "Scores are computed only from the candidate's virtual low-res texture and full-res tile position.",
      "When --lowres-source is supplied, scores are computed from that actual pre-composite low-res BMP.",
      "Reference/full-res data is used only to define oracle tiles and score the ranking.",
      "Single-frame success is not production proof; use this as a red/green direction filter before shader work.",
    ],
  };

  if (args.json) {
    console.log(JSON.stringify(output, null, 2));
  } else {
    console.log(
      `oracleTiles=${output.oracle.tileCount} oraclePixels=${output.oracle.selectedPixels} ` +
      `oraclePixelUpperPct=${output.oracle.repairedPctUpperBound.toFixed(4)} ` +
      `candidateBandTiles=${output.candidateTilesInBand} lowres=${bg.width}x${bg.height}`);
    for (const result of results.slice(0, 8)) {
      const minTiles = result.minTilesForFullOracle === null ? "misses-band" : String(result.minTilesForFullOracle);
      const budgetText = result.budgets
        .map((b) => `${b.budget}:${b.oracleMissed}/${b.repairedMaxDelta}`)
        .join(" ");
      console.log(
        `feature=${result.feature} minTilesForFullOracle=${minTiles} ` +
        `cutoffScore=${result.cutoffScoreForFullOracle ?? "na"} ` +
        `estimatedMs=${result.estimatedMsAtFullOracle === null ? "na" : result.estimatedMsAtFullOracle.toFixed(3)} ` +
        `targetMarginMs=${result.targetMarginMs === null ? "na" : result.targetMarginMs.toFixed(3)} ` +
        `falsePositiveTiles=${result.falsePositiveTilesAtFullOracle ?? "na"} budgets(missed/maxDelta)=${budgetText}`);
      if (result.thresholdSelection) {
        const t = result.thresholdSelection;
        console.log(
          `  threshold=${t.scoreThreshold} selectedTiles=${t.selectedTileCount} ` +
          `missed=${t.oracleMissed} pixelUpperPct=${t.selectedPixelUpperPct.toFixed(4)} ` +
          `estimatedMs=${t.estimatedMs === null ? "na" : t.estimatedMs.toFixed(3)} ` +
          `targetMarginMs=${t.targetMarginMs === null ? "na" : t.targetMarginMs.toFixed(3)} ` +
          `repairedMaxDelta=${t.repairedMaxDelta}`);
      }
    }
  }

  if (args.requireCoverBudget !== null) {
    const checkedResults = args.requireFeature
      ? results.filter((result) => result.feature === args.requireFeature)
      : results;
    const pass = checkedResults.some((result) => {
      if (args.requireScoreThreshold) {
        const t = result.thresholdSelection;
        return t !== null &&
          t.oracleMissed === 0 &&
          t.selectedTileCount <= args.requireCoverBudget &&
          (args.targetMs === undefined || t.targetMarginMs === null || t.targetMarginMs >= 0);
      }
      return result.minTilesForFullOracle !== null &&
        result.minTilesForFullOracle <= args.requireCoverBudget &&
        (args.targetMs === undefined || result.targetMarginMs === null || result.targetMarginMs >= 0);
    });
    if (!pass) {
      if (!args.json) {
        const featureText = args.requireFeature ? ` requireFeature=${args.requireFeature}` : "";
        const thresholdText = args.requireScoreThreshold ? ` requireScoreThreshold=${args.scoreThreshold}` : "";
        console.error(`HORIZON_DELTA_PREDICTOR ok=false requireCoverBudget=${args.requireCoverBudget}${featureText}${thresholdText}`);
      }
      process.exit(1);
    }
  }
  if (!args.json) {
    console.log("HORIZON_DELTA_PREDICTOR ok=true");
  }
}

main();
