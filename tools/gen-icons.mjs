// Renders the RinCast logo procedurally (anti-aliased SDF) and writes:
//   assets/brand/logo-<size>.png, assets/brand/rincast.ico
// Usage: node tools/gen-icons.mjs
import { deflateSync } from "node:zlib";
import { mkdirSync, writeFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const root = join(dirname(fileURLToPath(import.meta.url)), "..");
const out = join(root, "assets", "brand");
mkdirSync(out, { recursive: true });

const crcTable = new Uint32Array(256).map((_, n) => {
  let c = n;
  for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
  return c >>> 0;
});
const crc32 = (buf) => {
  let c = 0xffffffff;
  for (const b of buf) c = crcTable[(c ^ b) & 0xff] ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
};
function chunk(type, data) {
  const len = Buffer.alloc(4);
  len.writeUInt32BE(data.length);
  const td = Buffer.concat([Buffer.from(type), data]);
  const crc = Buffer.alloc(4);
  crc.writeUInt32BE(crc32(td));
  return Buffer.concat([len, td, crc]);
}
function png(size, rgba) {
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(size, 0);
  ihdr.writeUInt32BE(size, 4);
  ihdr[8] = 8; ihdr[9] = 6; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
  const raw = Buffer.alloc(size * (size * 4 + 1));
  for (let y = 0; y < size; y++) {
    raw[y * (size * 4 + 1)] = 0;
    rgba.copy(raw, y * (size * 4 + 1) + 1, y * size * 4, (y + 1) * size * 4);
  }
  return Buffer.concat([
    Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]),
    chunk("IHDR", ihdr),
    chunk("IDAT", deflateSync(raw, { level: 9 })),
    chunk("IEND", Buffer.alloc(0)),
  ]);
}

const clamp = (v, a = 0, b = 1) => Math.min(b, Math.max(a, v));
const mix = (a, b, t) => a + (b - a) * t;
const C1 = [157, 139, 255], C2 = [79, 209, 255], BG = [17, 17, 24];

// Signed distances in a 64x64 design space.
function sdRoundRect(x, y, cx, cy, hw, hh, r) {
  const qx = Math.abs(x - cx) - hw + r, qy = Math.abs(y - cy) - hh + r;
  return Math.hypot(Math.max(qx, 0), Math.max(qy, 0)) + Math.min(Math.max(qx, qy), 0) - r;
}
// Distance to a circular arc of radius r between angles a0..a1 (radians, y down).
function sdArc(x, y, r, a0, a1) {
  const ang = Math.atan2(y, x);
  if (ang >= a0 && ang <= a1) return Math.abs(Math.hypot(x, y) - r);
  const e0 = Math.hypot(x - r * Math.cos(a0), y - r * Math.sin(a0));
  const e1 = Math.hypot(x - r * Math.cos(a1), y - r * Math.sin(a1));
  return Math.min(e0, e1);
}

function render(size, { background = true } = {}) {
  const buf = Buffer.alloc(size * size * 4);
  const ss = 4; // supersampling
  for (let py = 0; py < size; py++) {
    for (let px = 0; px < size; px++) {
      let acc = [0, 0, 0, 0];
      for (let sy = 0; sy < ss; sy++) {
        for (let sx = 0; sx < ss; sx++) {
          const x = ((px + (sx + 0.5) / ss) / size) * 64;
          const y = ((py + (sy + 0.5) / ss) / size) * 64;
          let col = [0, 0, 0], a = 0;
          if (background) {
            const d = sdRoundRect(x, y, 32, 32, 28, 28, 16);
            if (d < 0) { col = BG; a = 1; }
          }
          const t = clamp((x + y) / 128 + 0.1);
          const grad = [mix(C1[0], C2[0], t), mix(C1[1], C2[1], t), mix(C1[2], C2[2], t)];
          const ring = Math.abs(Math.hypot(x - 32, y - 32) - 16.5) - 2.75;
          const dot = Math.hypot(x - 32, y - 32) - 6;
          // Broadcast arc hugging the ring's upper-right quadrant.
          const arc = sdArc(x - 32, y - 32, 23.5, -1.35, -0.22) - 2.4;
          if (Math.min(ring, dot, arc) < 0) { col = grad; a = 1; }
          acc[0] += col[0] * a; acc[1] += col[1] * a; acc[2] += col[2] * a; acc[3] += a;
        }
      }
      const n = ss * ss, i = (py * size + px) * 4;
      const alpha = acc[3] / n;
      buf[i] = alpha ? Math.round(acc[0] / acc[3]) : 0;
      buf[i + 1] = alpha ? Math.round(acc[1] / acc[3]) : 0;
      buf[i + 2] = alpha ? Math.round(acc[2] / acc[3]) : 0;
      buf[i + 3] = Math.round(alpha * 255);
    }
  }
  return buf;
}

const icoSizes = [16, 20, 24, 32, 40, 48, 64, 128, 256];
const images = icoSizes.map((s) => png(s, render(s)));
for (const s of [64, 128, 192, 256, 432, 512]) writeFileSync(join(out, `logo-${s}.png`), png(s, render(s)));
writeFileSync(join(out, "mark-256.png"), png(256, render(256, { background: false })));

// ICO container with PNG-compressed entries.
const header = Buffer.alloc(6);
header.writeUInt16LE(0, 0); header.writeUInt16LE(1, 2); header.writeUInt16LE(images.length, 4);
let offset = 6 + 16 * images.length;
const entries = images.map((img, i) => {
  const e = Buffer.alloc(16), s = icoSizes[i];
  e[0] = s >= 256 ? 0 : s; e[1] = s >= 256 ? 0 : s; e[2] = 0; e[3] = 0;
  e.writeUInt16LE(1, 4); e.writeUInt16LE(32, 6);
  e.writeUInt32LE(img.length, 8); e.writeUInt32LE(offset, 12);
  offset += img.length;
  return e;
});
writeFileSync(join(out, "rincast.ico"), Buffer.concat([header, ...entries, ...images]));
console.log("wrote", out);
