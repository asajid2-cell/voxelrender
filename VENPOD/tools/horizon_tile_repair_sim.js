#!/usr/bin/env node
"use strict";

const fs = require("fs");

function usage() {
  console.error([
    "usage: node tools/horizon_tile_repair_sim.js --reference <bmp> --candidate <bmp> [options]",
    "",
    "Options:",
    "  --threshold <n>      Per-channel delta threshold to select repair pixels (default 96)",
    "  --tiles <csv>        Tile sizes to simulate, in full-res pixels (default 4,8,12,16,24,32)",
    "  --json               Emit JSON only",
  ].join("\n"));
}

function parseArgs(argv) {
  const args = { threshold: 96, tiles: [4, 8, 12, 16, 24, 32], json: false };
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
    case "--threshold":
      args.threshold = Number(value);
      ++i;
      break;
    case "--tiles":
      args.tiles = value.split(",").map((v) => Number(v.trim())).filter((v) => Number.isFinite(v) && v > 0);
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
  if (!args.reference || !args.candidate || !Number.isFinite(args.threshold) || args.tiles.length === 0) {
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

function cloneImage(img) {
  return { ...img, data: Buffer.from(img.data) };
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

function maxChannelDelta(ref, cand, x, y) {
  const ro = pixelOffset(ref, x, y);
  const co = pixelOffset(cand, x, y);
  return Math.max(
    Math.abs(ref.data[ro + 0] - cand.data[co + 0]),
    Math.abs(ref.data[ro + 1] - cand.data[co + 1]),
    Math.abs(ref.data[ro + 2] - cand.data[co + 2]));
}

function selectTiles(ref, cand, tileSize, threshold) {
  const tiles = new Set();
  let selectedPixels = 0;
  for (let y = 0; y < ref.height; ++y) {
    for (let x = 0; x < ref.width; ++x) {
      if (maxChannelDelta(ref, cand, x, y) > threshold) {
        tiles.add(`${Math.floor(x / tileSize)},${Math.floor(y / tileSize)}`);
        ++selectedPixels;
      }
    }
  }
  return { tiles, selectedPixels };
}

function applyReferenceTiles(ref, cand, tiles, tileSize) {
  const repaired = cloneImage(cand);
  for (const tile of tiles) {
    const [tx, ty] = tile.split(",").map(Number);
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

function main() {
  const args = parseArgs(process.argv);
  const ref = readBmp24(args.reference);
  const cand = readBmp24(args.candidate);
  if (ref.width !== cand.width || ref.height !== cand.height) {
    throw new Error(`dimension mismatch: reference ${ref.width}x${ref.height}, candidate ${cand.width}x${cand.height}`);
  }
  const screenPixels = ref.width * ref.height;
  const before = metrics(ref, cand);
  const results = args.tiles.map((tileSize) => {
    const { tiles, selectedPixels } = selectTiles(ref, cand, tileSize, args.threshold);
    const repaired = applyReferenceTiles(ref, cand, tiles, tileSize);
    return {
      tileSize,
      selectedPixels,
      tileCount: tiles.size,
      repairedPixelUpperBound: tiles.size * tileSize * tileSize,
      repairedPctUpperBound: Math.round((1000000 * tiles.size * tileSize * tileSize) / screenPixels) / 10000,
      metrics: metrics(ref, repaired),
    };
  });
  const output = {
    reference: args.reference,
    candidate: args.candidate,
    threshold: args.threshold,
    dimensions: { width: ref.width, height: ref.height },
    before,
    results,
  };
  if (args.json) {
    console.log(JSON.stringify(output, null, 2));
  } else {
    console.log(JSON.stringify(output, null, 2));
  }
}

main();
