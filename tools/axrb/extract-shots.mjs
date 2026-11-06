// Decodes one PNG per UI state recorded by ui-tour.ps1.
//   node tools/axrb/extract-shots.mjs
import { execFileSync } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';

const out = path.resolve(import.meta.dirname, '../../build/axrb-test');
const progress = fs.readFileSync(path.join(out, 'sim.log'), 'utf8')
  .split('\n').map(l => l.match(/unix_ms=(\d+) stream=0 frames=(\d+)/)).filter(Boolean)
  .map(m => ({ ms: Number(m[1]), frames: Number(m[2]) }));
const states = fs.readFileSync(path.join(out, 'states.log'), 'utf8').trim().split('\n')
  .map(l => { const [name, ms] = l.trim().split(' '); return { name, ms: Number(ms) }; });

const select = [];
for (const s of states) {
  // Last progress sample at or before the state's timestamp, minus a short margin.
  const p = progress.filter(p => p.ms <= s.ms).at(-1);
  if (!p) continue;
  s.frame = Math.max(0, p.frames - 30);
  select.push(`eq(n\\,${s.frame})`);
}
fs.mkdirSync(path.join(out, 'shots'), { recursive: true });
execFileSync('ffmpeg', ['-v', 'error', '-y', '-i', path.join(out, 'sim', 'stream0.h265'),
  '-vf', `select='${select.join('+')}'`, '-vsync', 'vfr', path.join(out, 'shots', 'tmp_%02d.png')]);
states.filter(s => s.frame !== undefined).forEach((s, i) => {
  const from = path.join(out, 'shots', `tmp_${String(i + 1).padStart(2, '0')}.png`);
  if (fs.existsSync(from)) fs.renameSync(from, path.join(out, 'shots', `${String(i + 1).padStart(2, '0')}-${s.name}.png`));
  console.log(s.name, s.frame);
});
