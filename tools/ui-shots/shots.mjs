// Screenshots the streamer UI (with its design-preview mock) in headless Edge, in
// every page and state, plus a small window and Windows high contrast.
//   node tools/ui-shots/shots.mjs [out dir] [--only name,name]
// Also prints what a keyboard user reaches, in Tab order, and any console errors.
import { spawn } from "node:child_process";
import { createServer } from "node:http";
import { mkdirSync, readFileSync, writeFileSync, existsSync, rmSync } from "node:fs";
import { dirname, extname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { tmpdir } from "node:os";

const repo = resolve(dirname(fileURLToPath(import.meta.url)), "../..");
const args = process.argv.slice(2);
const onlyAt = args.indexOf("--only");
const only = onlyAt >= 0 ? args.splice(onlyAt, 2)[1].split(",") : null;
const out = resolve(args[0] || join(repo, "build/ui-shots"));
mkdirSync(out, { recursive: true });

const edge = [
  process.env["ProgramFiles(x86)"] + "\\Microsoft\\Edge\\Application\\msedge.exe",
  process.env.ProgramFiles + "\\Microsoft\\Edge\\Application\\msedge.exe",
].find(existsSync);

const types = { ".html": "text/html", ".css": "text/css", ".js": "text/javascript", ".png": "image/png", ".svg": "image/svg+xml" };
const uiDir = join(repo, "pc/streamer/ui");
const server = createServer((req, res) => {
  const path = new URL(req.url, "http://x").pathname;
  const file = join(uiDir, path === "/" ? "index.html" : path);
  if (!file.startsWith(uiDir) || !existsSync(file)) { res.writeHead(404); res.end(); return; }
  res.writeHead(200, { "Content-Type": types[extname(file)] || "application/octet-stream" });
  res.end(readFileSync(file));
});
await new Promise((r) => server.listen(0, "127.0.0.1", r));
const base = `http://127.0.0.1:${server.address().port}/`;

const profile = join(tmpdir(), `rincast-shots-${process.pid}`);
const browser = spawn(edge, ["--headless=new", "--remote-debugging-port=0", `--user-data-dir=${profile}`, "--no-first-run", "--hide-scrollbars", "about:blank"], { stdio: "ignore" });
const portFile = join(profile, "DevToolsActivePort");
let wsUrl = null;
for (let i = 0; i < 200 && !wsUrl; i++) {
  await new Promise((r) => setTimeout(r, 100));
  if (existsSync(portFile)) {
    const [port, path] = readFileSync(portFile, "utf8").split(String.fromCharCode(10));
    if (port && path) wsUrl = `ws://127.0.0.1:${port.trim()}${path.trim()}`;
  }
}
if (!wsUrl) throw new Error("Edge did not start");

const ws = new WebSocket(wsUrl);
await new Promise((r) => (ws.onopen = r));
let nextId = 1;
const waiting = new Map();
const consoleErrors = [];
ws.onmessage = (e) => {
  const m = JSON.parse(e.data);
  if (m.id && waiting.has(m.id)) {
    const { ok, fail } = waiting.get(m.id);
    waiting.delete(m.id);
    m.error ? fail(new Error(m.error.message)) : ok(m.result);
  } else if (m.method === "Runtime.exceptionThrown") {
    consoleErrors.push(m.params.exceptionDetails.exception?.description || m.params.exceptionDetails.text);
  } else if (m.method === "Runtime.consoleAPICalled" && m.params.type === "error") {
    consoleErrors.push(m.params.args.map((a) => a.value ?? a.description).join(" "));
  }
};
const send = (method, params = {}, sessionId) => new Promise((ok, fail) => {
  const id = nextId++;
  waiting.set(id, { ok, fail });
  ws.send(JSON.stringify({ id, method, params, sessionId }));
});
const { targetId } = await send("Target.createTarget", { url: "about:blank" });
const { sessionId } = await send("Target.attachToTarget", { targetId, flatten: true });
const page = (method, params) => send(method, params, sessionId);
await page("Page.enable");
await page("Runtime.enable");

const wait = (ms) => new Promise((r) => setTimeout(r, ms));
const js = async (expression) => (await page("Runtime.evaluate", { expression, awaitPromise: true, returnByValue: true })).result?.value;

async function shot(name, { demo = "connected", nav = "home", width = 900, height = 690, scale = 1, media = [], before, settle = 1300 } = {}) {
  if (only && !only.includes(name)) return;
  await page("Emulation.setDeviceMetricsOverride", { width, height, deviceScaleFactor: scale, mobile: false });
  await page("Emulation.setEmulatedMedia", { features: media });
  await page("Page.navigate", { url: `${base}?demo=${demo}` });
  await wait(500);
  await js(`document.querySelector('[data-page="${nav}"]')?.click()`);
  if (before) await js(before);
  await wait(settle);  // past entrance transitions and a stats tick
  const { data } = await page("Page.captureScreenshot", { format: "png" });
  writeFileSync(join(out, `${name}.png`), Buffer.from(data, "base64"));
  console.log(name);
}

await shot("home-connected");
await shot("home-idle", { demo: "idle" });
await shot("home-approval", { demo: "approval" });
await shot("home-firstrun", { demo: "firstrun" });
await shot("home-problem", { demo: "problem" });
await shot("home-lowbandwidth", { demo: "lowbandwidth", settle: 6500 });
await shot("account-signedout", { demo: "signedout", nav: "account" });
await shot("page-video-connected", { nav: "video" });
await shot("home-public-network", { demo: "public" });
await shot("home-vpn", { demo: "vpn" });
await shot("confirm-public-network", { demo: "public", nav: "general", before: `document.getElementById("allowPublic").click()` });
await shot("confirm-disconnect", { before: `[...document.querySelectorAll("#homeActions button")].find((b) => b.textContent === "Disconnect").click()` });
for (const p of ["video", "audio", "vr", "devices", "account", "general"]) await shot(`page-${p}`, { nav: p, demo: "idle" });
await shot("page-devices-empty", { nav: "devices", demo: "firstrun" });
await shot("page-register", { nav: "account", demo: "signedout", before: `document.getElementById("toggleAuth").click()` });
await shot("error-banner", { demo: "idle", before: `document.getElementById("banner").hidden = false; document.getElementById("bannerText").textContent = "Couldn't add RinCast to SteamVR. Start SteamVR once, then try again."` });
await shot("small-window-home", { width: 760, height: 560 });
await shot("small-window-video", { width: 760, height: 560, nav: "video", demo: "idle" });
await shot("high-contrast-home", { media: [{ name: "forced-colors", value: "active" }] });
await shot("high-contrast-video", { media: [{ name: "forced-colors", value: "active" }], nav: "video", demo: "idle" });
await shot("text-200-percent", { width: 430, height: 310, scale: 2, nav: "video", demo: "idle" });

if (!only) {
  // Walk the Tab order on Streaming: the focus indicator and accessible names.
  await page("Emulation.setDeviceMetricsOverride", { width: 900, height: 690, deviceScaleFactor: 1, mobile: false });
  await page("Emulation.setEmulatedMedia", { features: [] });
  await page("Page.navigate", { url: `${base}?demo=idle` });
  await wait(500);
  await js(`document.querySelector('[data-page="video"]').click()`);
  await js(`document.activeElement?.blur()`);
  const order = [];
  for (let i = 0; i < 24; i++) {
    await page("Input.dispatchKeyEvent", { type: "keyDown", key: "Tab", code: "Tab", windowsVirtualKeyCode: 9 });
    await page("Input.dispatchKeyEvent", { type: "keyUp", key: "Tab", code: "Tab", windowsVirtualKeyCode: 9 });
    const el = await js(`(() => { const e = document.activeElement; if (!e || e === document.body) return null;
      const s = getComputedStyle(e);
      return { tag: e.tagName.toLowerCase(), type: e.type || null, role: e.getAttribute('role'),
               name: e.getAttribute('aria-label') || e.labels?.[0]?.innerText?.split('\\n')[0] || e.innerText?.trim().slice(0, 40) || null,
               outline: s.outlineStyle !== 'none' && parseFloat(s.outlineWidth) > 0 }; })()`);
    if (!el) break;
    order.push(el);
    if (i === 3) {
      const { data } = await page("Page.captureScreenshot", { format: "png" });
      writeFileSync(join(out, "keyboard-focus.png"), Buffer.from(data, "base64"));
    }
  }
  writeFileSync(join(out, "tab-order.json"), JSON.stringify(order, null, 1));
  console.log(`tab order: ${order.length} stops, ${order.filter((e) => !e.outline).length} with no visible outline, ${order.filter((e) => !e.name).length} with no name`);
}
if (consoleErrors.length) console.log("console errors:\n" + [...new Set(consoleErrors)].join("\n"));

ws.close();
browser.kill();
server.close();
await wait(300);
try { rmSync(profile, { recursive: true, force: true, maxRetries: 5, retryDelay: 300 }); } catch {}
process.exit(0);
