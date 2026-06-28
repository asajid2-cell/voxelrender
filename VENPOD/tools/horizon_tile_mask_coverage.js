#!/usr/bin/env node
"use strict";

const fs = require("fs");

function usage() {
  console.error([
    "usage: node tools/horizon_tile_mask_coverage.js --reference <bmp> --candidate <bmp> [options]",
    "",
    "Options:",
    "  --tile-size <n>           Full-res tile size in pixels (default 8)",
    "  --visual-threshold <n>    Oracle per-channel delta threshold (default 96)",
    "  --edge-threshold <n>      Runtime-style low-res edge threshold, 0..1 (default 0.52)",
    "  --y0 <n>                  Runtime-style horizon band start row (default 320)",
    "  --y1 <n>                  Runtime-style horizon band end row (default 480)",
    "  --bg-scale <n>            Virtual low-res background scale (default 0.25)",
    "  --bg-width <n>            Override virtual low-res width",
    "  --bg-height <n>           Override virtual low-res height",
    "  --lowres <average|center> Virtual low-res reconstruction mode (default average)",
    "  --horizon-csv <csv>       FarScreenHorizonY CSV from VENPOD_FAR_MAX_HEIGHT_HORIZON_CSV",
    "  --horizon-above <n>       Pixels above horizonY to include for horizon selector (default 24)",
    "  --horizon-below <n>       Pixels below horizonY to include for horizon selector (default 8)",
    "  --sweep <csv>             Run multiple edge thresholds",
    "  --require-oracle-covered  Exit nonzero if any oracle tile is missed",
    "  --require-repaired-max <n> Exit nonzero if repaired maxDelta exceeds n",
    "  --json                    Emit JSON only",
  ].join("\n"));
}

function parseArgs(argv) {
  const args = {
    tileSize: 8,
    visualThreshold: 96,
    edgeThreshold: 0.52,
    y0: 320,
    y1: 480,
    bgScale: 0.25,
    lowres: "average",
    json: false,
    requireOracleCovered: false,
    requireRepairedMax: null,
    sweep: null,
    horizonAbove: 24,
    horizonBelow: 8,
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
    case "--tile-size":
      args.tileSize = Number(value);
      ++i;
      break;
    case "--visual-threshold":
      args.visualThreshold = Number(value);
      ++i;
      break;
    case "--edge-threshold":
      args.edgeThreshold = Number(value);
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
    case "--horizon-csv":
      args.horizonCsv = value;
      ++i;
      break;
    case "--horizon-above":
      args.horizonAbove = Number(value);
      ++i;
      break;
    case "--horizon-below":
      args.horizonBelow = Number(value);
      ++i;
      break;
    case "--sweep":
      args.sweep = value.split(",").map((v) => Number(v.trim())).filter(Number.isFinite);
      ++i;
      break;
    case "--require-oracle-covered":
      args.requireOracleCovered = true;
      break;
    case "--require-repaired-max":
      args.requireRepairedMax = Number(value);
      ++i;
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
      !Number.isFinite(args.edgeThreshold) ||
      !Number.isFinite(args.y0) || !Number.isFinite(args.y1) ||
      !Number.isFinite(args.horizonAbove) || args.horizonAbove < 0 ||
      !Number.isFinite(args.horizonBelow) || args.horizonBelow < 0 ||
      !Number.isFinite(args.bgScale) || args.bgScale <= 0 ||
      !["average", "center"].includes(args.lowres) ||
      (args.requireRepairedMax !== null && !Number.isFinite(args.requireRepairedMax))) {
    usage();
    process.exit(2);
  }
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

function cloneImage(img) {
  return { ...img, data: Buffer.from(img.data) };
}

function luma(rgb01) {
  return rgb01[0] * 0.2126 + rgb01[1] * 0.7152 + rgb01[2] * 0.0722;
}

function maxChannelDelta(ref, cand, x, y) {
  const ro = pixelOffset(ref, x, y);
  const co = pixelOffset(cand, x, y);
  return Math.max(
    Math.abs(ref.data[ro + 0] - cand.data[co + 0]),
    Math.abs(ref.data[ro + 1] - cand.data[co + 1]),
    Math.abs(ref.data[ro + 2] - cand.data[co + 2]));
}

function tileKey(tx, ty) {
  return `${tx},${ty}`;
}

function parseTileKey(key) {
  const [tx, ty] = key.split(",").map(Number);
  return { tx, ty };
}

function selectOracleTiles(ref, cand, tileSize, threshold) {
  const tiles = new Set();
  let selectedPixels = 0;
  for (let y = 0; y < ref.height; ++y) {
    for (let x = 0; x < ref.width; ++x) {
      if (maxChannelDelta(ref, cand, x, y) > threshold) {
        tiles.add(tileKey(Math.floor(x / tileSize), Math.floor(y / tileSize)));
        ++selectedPixels;
      }
    }
  }
  return { tiles, selectedPixels };
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

function lowresRgb(bg, x, y) {
  const o = (y * bg.width + x) * 3;
  return [bg.pixels[o + 0], bg.pixels[o + 1], bg.pixels[o + 2]];
}

function selectRuntimeStyleTiles(bg, fullWidth, fullHeight, tileSize, y0, y1, edgeThreshold) {
  const tiles = new Set();
  const tilesX = Math.ceil(Math.max(1, fullWidth) / tileSize);
  const tilesY = Math.ceil(Math.max(1, fullHeight) / tileSize);
  const bandY0 = Math.min(y0, Math.max(1, fullHeight));
  const bandY1 = Math.min(Math.max(y1, bandY0), Math.max(1, fullHeight));
  let bandTiles = 0;
  let maxEdge255 = 0;

  for (let ty = 0; ty < tilesY; ++ty) {
    const tileY0 = ty * tileSize;
    const tileY1 = Math.min(Math.max(1, fullHeight), tileY0 + tileSize);
    if (tileY1 <= bandY0 || tileY0 >= bandY1) {
      continue;
    }
    ++bandTiles;
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
      const minRgb = [1.0e9, 1.0e9, 1.0e9];
      const maxRgb = [-1.0e9, -1.0e9, -1.0e9];
      for (let y = sy0; y <= sy1; ++y) {
        for (let x = sx0; x <= sx1; ++x) {
          const rgb = lowresRgb(bg, x, y);
          const lum = luma(rgb);
          minLum = Math.min(minLum, lum);
          maxLum = Math.max(maxLum, lum);
          for (let k = 0; k < 3; ++k) {
            minRgb[k] = Math.min(minRgb[k], rgb[k]);
            maxRgb[k] = Math.max(maxRgb[k], rgb[k]);
          }
        }
      }

      const lumaRange = maxLum - minLum;
      const rgbRange = Math.max(maxRgb[0] - minRgb[0], maxRgb[1] - minRgb[1], maxRgb[2] - minRgb[2]);
      const edge = Math.max(lumaRange, rgbRange);
      maxEdge255 = Math.max(maxEdge255, Math.round(Math.max(0, Math.min(1, edge)) * 255.0));
      if (edge >= edgeThreshold) {
        tiles.add(tileKey(tx, ty));
      }
    }
  }

  return {
    tiles,
    bandTiles,
    maxEdge255,
    pixelUpper: tiles.size * tileSize * tileSize,
    totalTiles: tilesX * tilesY,
  };
}

function readHorizonCsv(filePath) {
  const text = fs.readFileSync(filePath, "utf8");
  const rows = [];
  const metadata = {};
  for (const rawLine of text.split(/\r?\n/)) {
    const line = rawLine.trim();
    if (!line) {
      continue;
    }
    if (line.startsWith("#")) {
      for (const match of line.matchAll(/([A-Za-z0-9_]+)=([^ \t]+)/g)) {
        metadata[match[1]] = match[2];
      }
      continue;
    }
    if (line === "tile,horizonY") {
      continue;
    }
    const parts = line.split(",");
    if (parts.length < 2) {
      continue;
    }
    const tile = Number(parts[0]);
    const horizonY = Number(parts[1]);
    if (Number.isFinite(tile) && Number.isFinite(horizonY)) {
      rows.push({ tile, horizonY });
    }
  }
  return {
    filePath,
    width: Number(metadata.width),
    height: Number(metadata.height),
    tileWidth: Number(metadata.tileWidth) || 8,
    tileCount: Number(metadata.tileCount) || rows.length,
    empty: Number(metadata.empty) || 0xffffffff,
    rows,
  };
}

function selectHorizonBandTiles(horizon, fullWidth, fullHeight, tileSize, abovePixels, belowPixels) {
  const tiles = new Set();
  let validHorizonTiles = 0;
  for (const row of horizon.rows) {
    if (row.horizonY === horizon.empty || row.horizonY > fullHeight + 1) {
      continue;
    }
    ++validHorizonTiles;
    const x0 = row.tile * horizon.tileWidth;
    const x1 = Math.min(fullWidth, x0 + horizon.tileWidth);
    const y0 = Math.max(0, Math.floor(row.horizonY - abovePixels));
    const y1 = Math.min(fullHeight, Math.ceil(row.horizonY + belowPixels));
    if (x1 <= x0 || y1 <= y0) {
      continue;
    }
    const tx0 = Math.floor(x0 / tileSize);
    const tx1 = Math.floor((x1 - 1) / tileSize);
    const ty0 = Math.floor(y0 / tileSize);
    const ty1 = Math.floor((y1 - 1) / tileSize);
    for (let ty = ty0; ty <= ty1; ++ty) {
      for (let tx = tx0; tx <= tx1; ++tx) {
        tiles.add(tileKey(tx, ty));
      }
    }
  }
  return {
    tiles,
    validHorizonTiles,
    pixelUpper: tiles.size * tileSize * tileSize,
  };
}

function applyReferenceTiles(ref, cand, tiles, tileSize) {
  const repaired = cloneImage(cand);
  for (const key of tiles) {
    const { tx, ty } = parseTileKey(key);
    for (let y = ty * tileSize; y < Math.min(ref.height, (ty + 1) * tileSize); ++y) {
      for (let x = tx * tileSize; x < Math.min(ref.width, (tx + 1) * tileSize); ++x) {
        const ro = pixelOffset(ref, x, y);
        const co = pixelOffset(repaired, x, y);
        repaired.data[co + 0] = ref.data[ro + 0];
        repaired.data[co + 1] = ref.data[ro + 1];
        repaired.data[co + 2] = ref.data[ro + 2];
      }
    }
  }
  return repaired;
}

function bandMetrics(ref, cand, y0Norm, y1Norm) {
  const y0 = Math.max(0, Math.min(ref.height, Math.round(ref.height * y0Norm)));
  const y1 = Math.max(y0, Math.min(ref.height, Math.round(ref.height * y1Norm)));
  let sum = 0;
  let maxDelta = 0;
  let samples = 0;
  for (let y = y0; y < y1; ++y) {
    for (let x = 0; x < ref.width; ++x) {
      const ro = pixelOffset(ref, x, y);
      const co = pixelOffset(cand, x, y);
      for (const k of [0, 1, 2]) {
        const delta = Math.abs(ref.data[ro + k] - cand.data[co + k]);
        sum += delta;
        maxDelta = Math.max(maxDelta, delta);
        ++samples;
      }
    }
  }
  return {
    mae: Math.round((sum / Math.max(samples, 1)) * 1000) / 1000,
    maxDelta,
  };
}

function metrics(ref, cand) {
  return {
    upperSky: bandMetrics(ref, cand, 0.08, 0.22),
    horizonSky: bandMetrics(ref, cand, 0.22, 0.38),
    farHorizonBand: bandMetrics(ref, cand, 0.17, 0.42),
    fullFrame: bandMetrics(ref, cand, 0.0, 1.0),
  };
}

function summarizeForThreshold(bg, ref, cand, args, edgeThreshold, oracle) {
  const runtime = selectRuntimeStyleTiles(
    bg,
    ref.width,
    ref.height,
    args.tileSize,
    args.y0,
    args.y1,
    edgeThreshold);

  const missed = [];
  for (const key of oracle.tiles) {
    if (!runtime.tiles.has(key)) {
      missed.push(key);
    }
  }

  const repaired = applyReferenceTiles(ref, cand, runtime.tiles, args.tileSize);
  const repairedMetrics = metrics(ref, repaired);
  return {
    edgeThreshold,
    oracleTileCount: oracle.tiles.size,
    runtimeTileCount: runtime.tiles.size,
    runtimeTotalTiles: runtime.totalTiles,
    runtimeBandTiles: runtime.bandTiles,
    runtimePixelUpper: runtime.pixelUpper,
    runtimePixelUpperPct: Math.round((1000000 * runtime.pixelUpper) / (ref.width * ref.height)) / 10000,
    runtimeMaxEdge255: runtime.maxEdge255,
    oracleTilesMissed: missed.length,
    oracleTilesCovered: oracle.tiles.size - missed.length,
    missedTiles: missed.slice(0, 32),
    repairedMetrics,
  };
}

function summarizeTileSet(ref, cand, args, oracle, tiles, extra = {}) {
  const missed = [];
  for (const key of oracle.tiles) {
    if (!tiles.has(key)) {
      missed.push(key);
    }
  }
  const repaired = applyReferenceTiles(ref, cand, tiles, args.tileSize);
  return {
    ...extra,
    oracleTileCount: oracle.tiles.size,
    selectedTileCount: tiles.size,
    selectedPixelUpper: tiles.size * args.tileSize * args.tileSize,
    selectedPixelUpperPct: Math.round((1000000 * tiles.size * args.tileSize * args.tileSize) / (ref.width * ref.height)) / 10000,
    oracleTilesMissed: missed.length,
    oracleTilesCovered: oracle.tiles.size - missed.length,
    missedTiles: missed.slice(0, 32),
    repairedMetrics: metrics(ref, repaired),
  };
}

function main() {
  const args = parseArgs(process.argv);
  const ref = readBmp24(args.reference);
  const cand = readBmp24(args.candidate);
  if (ref.width !== cand.width || ref.height !== cand.height) {
    throw new Error(`dimension mismatch: reference ${ref.width}x${ref.height}, candidate ${cand.width}x${cand.height}`);
  }

  const oracle = selectOracleTiles(ref, cand, args.tileSize, args.visualThreshold);
  const bg = buildVirtualLowres(cand, args);
  const thresholds = args.sweep && args.sweep.length > 0 ? args.sweep : [args.edgeThreshold];
  const results = thresholds.map((threshold) => summarizeForThreshold(bg, ref, cand, args, threshold, oracle));
  let horizonResult = null;
  if (args.horizonCsv) {
    const horizon = readHorizonCsv(args.horizonCsv);
    const selected = selectHorizonBandTiles(
      horizon,
      ref.width,
      ref.height,
      args.tileSize,
      args.horizonAbove,
      args.horizonBelow);
    horizonResult = summarizeTileSet(ref, cand, args, oracle, selected.tiles, {
      horizonCsv: args.horizonCsv,
      horizonAbove: args.horizonAbove,
      horizonBelow: args.horizonBelow,
      horizonTileWidth: horizon.tileWidth,
      validHorizonTiles: selected.validHorizonTiles,
    });
  }
  const output = {
    reference: args.reference,
    candidate: args.candidate,
    dimensions: { width: ref.width, height: ref.height },
    tileSize: args.tileSize,
    visualThreshold: args.visualThreshold,
    horizonBand: { y0: args.y0, y1: args.y1 },
    lowres: { mode: args.lowres, width: bg.width, height: bg.height },
    before: metrics(ref, cand),
    oracle: {
      selectedPixels: oracle.selectedPixels,
      tileCount: oracle.tiles.size,
      repairedPixelUpperBound: oracle.tiles.size * args.tileSize * args.tileSize,
      repairedPctUpperBound: Math.round((1000000 * oracle.tiles.size * args.tileSize * args.tileSize) / (ref.width * ref.height)) / 10000,
    },
    results,
    horizonResult,
  };

  if (!args.json) {
    for (const r of results) {
      console.log(
        `threshold=${r.edgeThreshold.toFixed(3)} runtimeTiles=${r.runtimeTileCount} ` +
        `oracleTiles=${r.oracleTileCount} missed=${r.oracleTilesMissed} ` +
        `pixelUpperPct=${r.runtimePixelUpperPct.toFixed(4)} maxEdge255=${r.runtimeMaxEdge255} ` +
        `repairedMaxDelta=${r.repairedMetrics.fullFrame.maxDelta}`);
    }
    if (horizonResult) {
      console.log(
        `horizonBand above=${horizonResult.horizonAbove} below=${horizonResult.horizonBelow} ` +
        `selectedTiles=${horizonResult.selectedTileCount} oracleTiles=${horizonResult.oracleTileCount} ` +
        `missed=${horizonResult.oracleTilesMissed} pixelUpperPct=${horizonResult.selectedPixelUpperPct.toFixed(4)} ` +
        `repairedMaxDelta=${horizonResult.repairedMetrics.fullFrame.maxDelta}`);
    }
  } else {
    console.log(JSON.stringify(output, null, 2));
  }

  const selected = results[results.length - 1];
  let failed = false;
  if (args.requireOracleCovered && selected.oracleTilesMissed !== 0) {
    failed = true;
  }
  if (args.requireRepairedMax !== null && selected.repairedMetrics.fullFrame.maxDelta > args.requireRepairedMax) {
    failed = true;
  }
  if (failed) {
    if (!args.json) {
      console.error("HORIZON_TILE_MASK_COVERAGE ok=false");
    }
    process.exit(1);
  }
  if (!args.json) {
    console.log("HORIZON_TILE_MASK_COVERAGE ok=true");
  }
}

main();
