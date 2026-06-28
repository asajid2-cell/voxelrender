#!/usr/bin/env node
"use strict";

const fs = require("fs");
const path = require("path");

function usage() {
  console.error([
    "usage: node tools/far_horizon_visual_check.js --reference <bmp> --candidate <bmp> [options]",
    "",
    "Options:",
    "  --full-mae-max <n>       Full-frame mean absolute error threshold (default 4.0)",
    "  --band-mae-max <n>       Per-band mean absolute error threshold (default 8.0)",
    "  --band-channel-max <n>   Per-band average channel bias threshold (default 12.0)",
    "  --pixel-max <n>          Per-channel max absolute pixel delta threshold (default 96)",
    "  --json                   Emit JSON only",
  ].join("\n"));
}

function parseArgs(argv) {
  const args = {
    fullMaeMax: 4.0,
    bandMaeMax: 8.0,
    bandChannelMax: 12.0,
    pixelMax: 96,
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
    case "--full-mae-max":
      args.fullMaeMax = Number(value);
      ++i;
      break;
    case "--band-mae-max":
      args.bandMaeMax = Number(value);
      ++i;
      break;
    case "--band-channel-max":
      args.bandChannelMax = Number(value);
      ++i;
      break;
    case "--pixel-max":
      args.pixelMax = Number(value);
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
  if (!args.reference || !args.candidate) {
    usage();
    process.exit(2);
  }
  for (const numeric of ["fullMaeMax", "bandMaeMax", "bandChannelMax", "pixelMax"]) {
    if (!Number.isFinite(args[numeric])) {
      throw new Error(`invalid numeric argument: ${numeric}`);
    }
  }
  return args;
}

function readBmp24(filePath) {
  const data = fs.readFileSync(filePath);
  if (data.length < 54 || data.toString("ascii", 0, 2) !== "BM") {
    throw new Error(`${filePath}: not a BMP file`);
  }
  const offBits = data.readUInt32LE(10);
  const dibSize = data.readUInt32LE(14);
  const width = data.readInt32LE(18);
  const heightRaw = data.readInt32LE(22);
  const planes = data.readUInt16LE(26);
  const bpp = data.readUInt16LE(28);
  const compression = data.readUInt32LE(30);
  if (dibSize !== 40 || planes !== 1 || bpp !== 24 || compression !== 0) {
    throw new Error(`${filePath}: expected uncompressed BITMAPINFOHEADER 24-bit BMP`);
  }
  if (width <= 0 || heightRaw === 0) {
    throw new Error(`${filePath}: invalid dimensions`);
  }

  const topDown = heightRaw < 0;
  const height = Math.abs(heightRaw);
  const stride = Math.floor((width * 3 + 3) / 4) * 4;
  if (offBits + stride * height > data.length) {
    throw new Error(`${filePath}: truncated pixel data`);
  }
  return { filePath, data, offBits, width, height, stride, topDown };
}

function pixelOffset(img, x, yTop) {
  const yStorage = img.topDown ? yTop : img.height - 1 - yTop;
  return img.offBits + yStorage * img.stride + x * 3;
}

function makeBands(width, height) {
  const clampY = (v) => Math.max(0, Math.min(height, Math.round(v)));
  const band = (name, y0, y1) => ({
    name,
    x0: 0,
    x1: width,
    y0: clampY(y0),
    y1: Math.max(clampY(y0), clampY(y1)),
  });
  return [
    band("upper_sky", height * 0.08, height * 0.22),
    band("horizon_sky", height * 0.22, height * 0.38),
    band("far_horizon_band", height * 0.17, height * 0.42),
    band("mid_terrain_control", height * 0.42, height * 0.68),
    band("full_frame", 0, height),
  ];
}

function compareBand(ref, cand, band) {
  let samples = 0;
  let absSum = 0;
  let maxDelta = 0;
  const refSum = [0, 0, 0];
  const candSum = [0, 0, 0];
  const biasSum = [0, 0, 0];

  for (let y = band.y0; y < band.y1; ++y) {
    for (let x = band.x0; x < band.x1; ++x) {
      const ro = pixelOffset(ref, x, y);
      const co = pixelOffset(cand, x, y);
      const rb = ref.data[ro + 0];
      const rg = ref.data[ro + 1];
      const rr = ref.data[ro + 2];
      const cb = cand.data[co + 0];
      const cg = cand.data[co + 1];
      const cr = cand.data[co + 2];
      const rd = Math.abs(rr - cr);
      const gd = Math.abs(rg - cg);
      const bd = Math.abs(rb - cb);
      absSum += rd + gd + bd;
      maxDelta = Math.max(maxDelta, rd, gd, bd);
      refSum[0] += rr;
      refSum[1] += rg;
      refSum[2] += rb;
      candSum[0] += cr;
      candSum[1] += cg;
      candSum[2] += cb;
      biasSum[0] += cr - rr;
      biasSum[1] += cg - rg;
      biasSum[2] += cb - rb;
      ++samples;
    }
  }
  const denom = Math.max(samples, 1);
  return {
    name: band.name,
    rect: { x0: band.x0, y0: band.y0, x1: band.x1, y1: band.y1 },
    pixels: samples,
    mae: absSum / (denom * 3),
    maxDelta,
    referenceAvgRgb: refSum.map((v) => v / denom),
    candidateAvgRgb: candSum.map((v) => v / denom),
    biasRgb: biasSum.map((v) => v / denom),
  };
}

function roundMetric(value) {
  return Math.round(value * 1000) / 1000;
}

function roundedBand(band) {
  return {
    ...band,
    mae: roundMetric(band.mae),
    referenceAvgRgb: band.referenceAvgRgb.map(roundMetric),
    candidateAvgRgb: band.candidateAvgRgb.map(roundMetric),
    biasRgb: band.biasRgb.map(roundMetric),
  };
}

function main() {
  const args = parseArgs(process.argv);
  const ref = readBmp24(args.reference);
  const cand = readBmp24(args.candidate);
  if (ref.width !== cand.width || ref.height !== cand.height) {
    throw new Error(`dimension mismatch: reference ${ref.width}x${ref.height}, candidate ${cand.width}x${cand.height}`);
  }

  const bands = makeBands(ref.width, ref.height).map((band) => compareBand(ref, cand, band));
  const full = bands.find((band) => band.name === "full_frame");
  const checkedBands = bands.filter((band) =>
    band.name !== "full_frame" && band.name !== "mid_terrain_control");
  const failures = [];

  if (full.mae > args.fullMaeMax) {
    failures.push(`full_frame mae ${roundMetric(full.mae)} > ${args.fullMaeMax}`);
  }
  for (const band of checkedBands) {
    const maxChannelBias = Math.max(...band.biasRgb.map((v) => Math.abs(v)));
    if (band.mae > args.bandMaeMax) {
      failures.push(`${band.name} mae ${roundMetric(band.mae)} > ${args.bandMaeMax}`);
    }
    if (band.maxDelta > args.pixelMax) {
      failures.push(`${band.name} maxDelta ${band.maxDelta} > ${args.pixelMax}`);
    }
    if (maxChannelBias > args.bandChannelMax) {
      failures.push(`${band.name} avg channel bias ${roundMetric(maxChannelBias)} > ${args.bandChannelMax}`);
    }
  }

  const result = {
    ok: failures.length === 0,
    reference: path.resolve(args.reference),
    candidate: path.resolve(args.candidate),
    width: ref.width,
    height: ref.height,
    thresholds: {
      fullMaeMax: args.fullMaeMax,
      bandMaeMax: args.bandMaeMax,
      bandChannelMax: args.bandChannelMax,
      pixelMax: args.pixelMax,
    },
    failures,
    bands: bands.map(roundedBand),
  };

  if (args.json) {
    console.log(JSON.stringify(result, null, 2));
  } else {
    console.log(`FAR_HORIZON_VISUAL ok=${result.ok} size=${ref.width}x${ref.height}`);
    for (const band of result.bands) {
      console.log(
        `${band.name}: mae=${band.mae} maxDelta=${band.maxDelta} ` +
        `refAvgRgb=${band.referenceAvgRgb.join(",")} candAvgRgb=${band.candidateAvgRgb.join(",")} ` +
        `biasRgb=${band.biasRgb.join(",")}`);
    }
    for (const failure of failures) {
      console.error(`FAIL ${failure}`);
    }
  }

  process.exit(result.ok ? 0 : 1);
}

try {
  main();
} catch (error) {
  console.error(error && error.stack ? error.stack : String(error));
  process.exit(2);
}
