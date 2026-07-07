#!/usr/bin/env node
// Generate the debug_text bitmap font atlas and coordinate table.

import fs from "node:fs";
import path from "node:path";
import process from "node:process";
import { fileURLToPath } from "node:url";
import zlib from "node:zlib";

const CELL_W = 8;
const CELL_H = 16;
const COLUMNS = 16;
const FIRST_CODE = 32;
const LAST_CODE = 126;

const GLYPHS = new Map(
  Object.entries({
    32: "00000/00000/00000/00000/00000/00000/00000",
    33: "00100/00100/00100/00100/00100/00000/00100",
    34: "01010/01010/01010/00000/00000/00000/00000",
    35: "01010/11111/01010/01010/11111/01010/01010",
    36: "00100/01111/10100/01110/00101/11110/00100",
    37: "11001/11010/00100/01000/10110/00110/00000",
    38: "01100/10010/10100/01000/10101/10010/01101",
    39: "00100/00100/01000/00000/00000/00000/00000",
    40: "00010/00100/01000/01000/01000/00100/00010",
    41: "01000/00100/00010/00010/00010/00100/01000",
    42: "00000/10101/01110/11111/01110/10101/00000",
    43: "00000/00100/00100/11111/00100/00100/00000",
    44: "00000/00000/00000/00000/00100/00100/01000",
    45: "00000/00000/00000/11111/00000/00000/00000",
    46: "00000/00000/00000/00000/00000/00100/00100",
    47: "00001/00010/00100/01000/10000/00000/00000",
    48: "01110/10001/10011/10101/11001/10001/01110",
    49: "00100/01100/00100/00100/00100/00100/01110",
    50: "01110/10001/00001/00010/00100/01000/11111",
    51: "11110/00001/00001/01110/00001/00001/11110",
    52: "00010/00110/01010/10010/11111/00010/00010",
    53: "11111/10000/11110/00001/00001/10001/01110",
    54: "00110/01000/10000/11110/10001/10001/01110",
    55: "11111/00001/00010/00100/01000/01000/01000",
    56: "01110/10001/10001/01110/10001/10001/01110",
    57: "01110/10001/10001/01111/00001/00010/01100",
    58: "00000/00100/00100/00000/00100/00100/00000",
    59: "00000/00100/00100/00000/00100/00100/01000",
    60: "00010/00100/01000/10000/01000/00100/00010",
    61: "00000/00000/11111/00000/11111/00000/00000",
    62: "01000/00100/00010/00001/00010/00100/01000",
    63: "01110/10001/00001/00010/00100/00000/00100",
    64: "01110/10001/10111/10101/10111/10000/01110",
    65: "01110/10001/10001/11111/10001/10001/10001",
    66: "11110/10001/10001/11110/10001/10001/11110",
    67: "01110/10001/10000/10000/10000/10001/01110",
    68: "11110/10001/10001/10001/10001/10001/11110",
    69: "11111/10000/10000/11110/10000/10000/11111",
    70: "11111/10000/10000/11110/10000/10000/10000",
    71: "01110/10001/10000/10111/10001/10001/01110",
    72: "10001/10001/10001/11111/10001/10001/10001",
    73: "01110/00100/00100/00100/00100/00100/01110",
    74: "00111/00010/00010/00010/00010/10010/01100",
    75: "10001/10010/10100/11000/10100/10010/10001",
    76: "10000/10000/10000/10000/10000/10000/11111",
    77: "10001/11011/10101/10101/10001/10001/10001",
    78: "10001/11001/10101/10011/10001/10001/10001",
    79: "01110/10001/10001/10001/10001/10001/01110",
    80: "11110/10001/10001/11110/10000/10000/10000",
    81: "01110/10001/10001/10001/10101/10010/01101",
    82: "11110/10001/10001/11110/10100/10010/10001",
    83: "01111/10000/10000/01110/00001/00001/11110",
    84: "11111/00100/00100/00100/00100/00100/00100",
    85: "10001/10001/10001/10001/10001/10001/01110",
    86: "10001/10001/10001/10001/01010/01010/00100",
    87: "10001/10001/10001/10101/10101/10101/01010",
    88: "10001/01010/00100/00100/00100/01010/10001",
    89: "10001/01010/00100/00100/00100/00100/00100",
    90: "11111/00001/00010/00100/01000/10000/11111",
    91: "01110/01000/01000/01000/01000/01000/01110",
    92: "10000/01000/00100/00010/00001/00000/00000",
    93: "01110/00010/00010/00010/00010/00010/01110",
    94: "00100/01010/10001/00000/00000/00000/00000",
    95: "00000/00000/00000/00000/00000/00000/11111",
    96: "01000/00100/00010/00000/00000/00000/00000",
    123: "00010/00100/00100/01000/00100/00100/00010",
    124: "00100/00100/00100/00100/00100/00100/00100",
    125: "01000/00100/00100/00010/00100/00100/01000",
    126: "00000/00000/01000/10101/00010/00000/00000",
  }).map(([code, rows]) => [Number(code), rows.split("/")]),
);

function glyphRows(code) {
  if (GLYPHS.has(code)) return GLYPHS.get(code);
  const ch = String.fromCodePoint(code);
  const upper = ch.toUpperCase().codePointAt(0);
  if (GLYPHS.has(upper)) return GLYPHS.get(upper);

  const rows = [];
  for (let y = 0; y < 7; y += 1) {
    let row = "";
    for (let x = 0; x < 5; x += 1) {
      const bit = (code + x * 17 + y * 31) >> ((x + y) % 7);
      row += bit & 1 ? "1" : "0";
    }
    rows.push(row);
  }
  return rows;
}

function setPixel(pixels, width, x, y, rgba) {
  const offset = (y * width + x) * 4;
  pixels[offset] = rgba[0];
  pixels[offset + 1] = rgba[1];
  pixels[offset + 2] = rgba[2];
  pixels[offset + 3] = rgba[3];
}

function drawGlyph(pixels, atlasW, cellX, cellY, code) {
  const rows = glyphRows(code);
  const originX = cellX * CELL_W + 1;
  const originY = cellY * CELL_H + 1;
  for (let y = 0; y < rows.length; y += 1) {
    const row = rows[y];
    for (let x = 0; x < row.length; x += 1) {
      if (row[x] !== "1") continue;
      const px = originX + x;
      const py = originY + y * 2;
      setPixel(pixels, atlasW, px, py, [255, 255, 255, 255]);
      setPixel(pixels, atlasW, px, py + 1, [255, 255, 255, 255]);
    }
  }
}

function crc32(buffer) {
  let crc = 0xffffffff;
  for (const byte of buffer) {
    crc ^= byte;
    for (let i = 0; i < 8; i += 1) {
      crc = crc & 1 ? (crc >>> 1) ^ 0xedb88320 : crc >>> 1;
    }
  }
  return (crc ^ 0xffffffff) >>> 0;
}

function pngChunk(kind, data) {
  const kindBytes = Buffer.from(kind, "ascii");
  const chunk = Buffer.concat([kindBytes, data]);
  const out = Buffer.alloc(12 + data.length);
  out.writeUInt32BE(data.length, 0);
  kindBytes.copy(out, 4);
  data.copy(out, 8);
  out.writeUInt32BE(crc32(chunk), 8 + data.length);
  return out;
}

function writePng(filePath, width, height, pixels) {
  const stride = width * 4;
  const rows = [];
  for (let y = 0; y < height; y += 1) {
    rows.push(Buffer.from([0]));
    rows.push(Buffer.from(pixels.subarray(y * stride, (y + 1) * stride)));
  }

  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(width, 0);
  ihdr.writeUInt32BE(height, 4);
  ihdr[8] = 8;
  ihdr[9] = 6;
  ihdr[10] = 0;
  ihdr[11] = 0;
  ihdr[12] = 0;

  const png = Buffer.concat([
    Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]),
    pngChunk("IHDR", ihdr),
    pngChunk("IDAT", zlib.deflateSync(Buffer.concat(rows), { level: 9 })),
    pngChunk("IEND", Buffer.alloc(0)),
  ]);
  fs.writeFileSync(filePath, png);
}

function main() {
  const outDir = path.dirname(fileURLToPath(import.meta.url));
  const atlasW = COLUMNS * CELL_W;
  const atlasRows = Math.ceil((LAST_CODE - FIRST_CODE + 1) / COLUMNS);
  const atlasH = atlasRows * CELL_H;
  const pixels = new Uint8Array(atlasW * atlasH * 4);
  const glyphs = [];

  for (let code = FIRST_CODE; code <= LAST_CODE; code += 1) {
    const index = code - FIRST_CODE;
    const cellX = index % COLUMNS;
    const cellY = Math.floor(index / COLUMNS);
    drawGlyph(pixels, atlasW, cellX, cellY, code);
    glyphs.push({
      code,
      x: cellX * CELL_W,
      y: cellY * CELL_H,
      w: CELL_W,
      h: CELL_H,
      advance: CELL_W,
    });
  }

  writePng(path.join(outDir, "debug_text_font.png"), atlasW, atlasH, pixels);
  fs.writeFileSync(
    path.join(outDir, "debug_text_font.json"),
    `${JSON.stringify(
      {
        schema: "pelican.debug_text_font",
        version: 1,
        atlas: "engine://debug_text_font.png",
        atlas_width: atlasW,
        atlas_height: atlasH,
        cell_width: CELL_W,
        cell_height: CELL_H,
        first_code: FIRST_CODE,
        last_code: LAST_CODE,
        glyphs,
      },
      null,
      2,
    )}\n`,
    "utf8",
  );
}

try {
  main();
} catch (error) {
  console.error(error);
  process.exitCode = 1;
}
