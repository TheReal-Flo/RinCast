// Screenshots the account website against a throwaway local server.
//   node tools/ui-shots/web-shots.mjs <rincast-server.exe> [out dir]
import { spawn } from "node:child_process";
import { mkdirSync, writeFileSync, existsSync, rmSync, readFileSync, mkdtempSync } from "node:fs";
import { join, resolve } from "node:path";
import { tmpdir } from "node:os";
const [serverExe, outArg] = process.argv.slice(2);
const out = resolve(outArg || "build/ui-shots-web");
mkdirSync(out, { recursive: true });
const data = mkdtempSync(join(tmpdir(), "rincast-web-"));
const port = 18099;
const base = `http://127.0.0.1:${port}`;
const server = spawn(serverExe, ["-http", `127.0.0.1:${port}`, "-udp", "127.0.0.1:18721", "-data", data, "-public-url", base], { stdio: "ignore" });
const wait = (ms) => new Promise((r) => setTimeout(r, ms));
await wait(1200);
const edge = [process.env["ProgramFiles(x86)"], "Microsoft", "Edge", "Application", "msedge.exe"].join(String.fromCharCode(92));
const profile = mkdtempSync(join(tmpdir(), "rincast-webshots-"));
const browser = spawn(edge, ["--headless=new", "--remote-debugging-port=0", `--user-data-dir=${profile}`, "--no-first-run", "--hide-scrollbars", "about:blank"], { stdio: "ignore" });
let wsUrl = null;
for (let i = 0; i < 200 && !wsUrl; i++) {
  await wait(100);
  const f = join(profile, "DevToolsActivePort");
  if (existsSync(f)) { const [p, path] = readFileSync(f, "utf8").split(String.fromCharCode(10)); if (p && path) wsUrl = `ws://127.0.0.1:${p.trim()}${path.trim()}`; }
}
const ws = new WebSocket(wsUrl);
await new Promise((r) => (ws.onopen = r));
let id = 1; const waiting = new Map();
const problems = [];
ws.onmessage = (e) => {
  const m = JSON.parse(e.data);
  if (m.id && waiting.has(m.id)) { waiting.get(m.id)(m.result ?? {}); waiting.delete(m.id); }
  else if (m.method === "Log.entryAdded" && m.params.entry.level === "error") problems.push(m.params.entry.text);
  else if (m.method === "Runtime.exceptionThrown") problems.push(m.params.exceptionDetails.exception?.description || m.params.exceptionDetails.text);
};
const send = (method, params = {}, sessionId) => new Promise((ok) => { const i = id++; waiting.set(i, ok); ws.send(JSON.stringify({ id: i, method, params, sessionId })); });
const { targetId } = await send("Target.createTarget", { url: "about:blank" });
const { sessionId } = await send("Target.attachToTarget", { targetId, flatten: true });
const page = (m, p) => send(m, p, sessionId);
await page("Page.enable");
await page("Log.enable");
await page("Runtime.enable");
const js = async (expression) => (await page("Runtime.evaluate", { expression, awaitPromise: true, returnByValue: true })).result?.value;
async function shot(name, path, { width = 1100, height = 760, before } = {}) {
  await page("Emulation.setDeviceMetricsOverride", { width, height, deviceScaleFactor: 1, mobile: width < 600 });
  if (path) await page("Page.navigate", { url: base + path });
  await wait(700);
  if (before) await js(before);
  await wait(500);
  const { data: png } = await page("Page.captureScreenshot", { format: "png" });
  writeFileSync(join(out, `${name}.png`), Buffer.from(png, "base64"));
  console.log(name);
}
await shot("web-landing", "/");
await shot("web-landing-phone", "/", { width: 390, height: 780 });
await shot("web-login", "/login");
await shot("web-register", "/register");
await shot("web-login-phone", "/login", { width: 390, height: 780 });
await shot("web-login-error", "/login", { before: `(() => { document.querySelector('[name=email]').value='nobody@example.com'; document.querySelector('[name=password]').value='wrong-password'; document.querySelector('form').requestSubmit(); })()` });
await page("Page.navigate", { url: base + "/" });
await wait(500);
await js(`fetch('/api/v1/auth/register',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({name:'Sam',email:'sam@example.com',password:'demo-password-1',deviceKind:'web'})}).then(r=>r.json()).then(t=>{localStorage.setItem('rc.access',t.accessToken);localStorage.setItem('rc.refresh',t.refreshToken)})`);
await shot("web-link", "/link");
await shot("web-account-empty", "/account");
await shot("web-account-phone", "/account", { width: 390, height: 780 });
await shot("web-settings", "/settings", { height: 860 });
await shot("web-link-confirm", "/link?code=ABCD-EFGH", { before: `document.querySelector("form").requestSubmit()` });
console.log(problems.length ? "browser errors:\n" + [...new Set(problems)].join("\n") : "no browser errors");
ws.close(); browser.kill(); server.kill();
await wait(500);
try { rmSync(profile, { recursive: true, force: true }); rmSync(data, { recursive: true, force: true }); } catch {}
process.exit(0);
