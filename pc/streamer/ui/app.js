// RinCast Streamer UI. Talks to the native app through WebView2 messaging;
// outside WebView2 (design preview) a small mock backend is used.
// Strings live in i18n.js; static HTML text carries data-i18n attributes.

import { LANGUAGES, applyI18n, detectLanguage, getLanguage, getLanguageChoice, initLanguage, locale, setLanguage, t } from "./i18n.js";

const $ = (s, r = document) => r.querySelector(s);
const $$ = (s, r = document) => [...r.querySelectorAll(s)];
const esc = (s) => String(s ?? "").replace(/[&<>"']/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));

initLanguage();
applyI18n();

// ---------------------------------------------------------------- bridge
const native = window.chrome?.webview;
const pending = new Map();
let nextId = 1;
const listeners = [];

function send(msg) {
  if (native) native.postMessage(JSON.stringify(msg));
  else mock(msg);
}
// Resolves with the reply, or with { ok: false } if the app doesn't answer in time.
function request(msg, timeoutMs = 30000) {
  const requestId = String(nextId++);
  return new Promise((resolve) => {
    const timer = setTimeout(() => {
      pending.delete(requestId);
      resolve({ ok: false, error: t("err.noResponse") });
    }, timeoutMs);
    pending.set(requestId, (r) => { clearTimeout(timer); resolve(r); });
    send({ ...msg, requestId });
  });
}
function receive(msg) {
  if (msg.requestId && pending.has(msg.requestId)) {
    pending.get(msg.requestId)(msg);
    pending.delete(msg.requestId);
    if (msg.type === "reply") return;
  }
  listeners.forEach((fn) => fn(msg));
}
if (native) native.addEventListener("message", (e) => receive(typeof e.data === "string" ? JSON.parse(e.data) : e.data));

// ---------------------------------------------------------------- state and notices
let state = null;
const history = [];

function toast(text) {
  const el = $("#toast");
  el.textContent = text;
  el.classList.add("show");
  clearTimeout(toast.timer);
  toast.timer = setTimeout(() => el.classList.remove("show"), 2600);
}
// Failures stay until dismissed: nobody should have to catch them in a toast.
function showError(text) {
  $("#bannerText").textContent = text;
  $("#banner").hidden = false;
  $("#main").scrollTop = 0;
}
$("#bannerClose").onclick = () => { $("#banner").hidden = true; };

const confirmDialog = $("#confirm");
function confirmAction(title, text, action, danger = true) {
  $("#confirmTitle").textContent = title;
  $("#confirmText").textContent = text;
  $("#confirmYes").textContent = action;
  $("#confirmYes").className = danger ? "btn danger" : "btn primary";
  confirmDialog.returnValue = "";
  confirmDialog.showModal();
  return new Promise((resolve) => {
    $("#confirmYes").onclick = () => confirmDialog.close("yes");
    $("#confirmNo").onclick = () => confirmDialog.close("no");
    confirmDialog.addEventListener("close", () => resolve(confirmDialog.returnValue === "yes"), { once: true });
  });
}

listeners.push((msg) => {
  switch (msg.type) {
    case "state":
      state = msg;
      renderAll();
      break;
    case "stats":
      if (state) state.session = msg.session;
      if (msg.session) countPoor(msg.session);
      renderHome();
      renderStatus();
      break;
    case "session":
      request({ type: "getState" });
      if (msg.connected) toast(t("toast.connected", { name: msg.deviceName }));
      else if (msg.reason !== "replaced") toast(t("toast.disconnected", { name: msg.deviceName }));
      break;
    case "screenshots":
      break;  // the list lives in the folder, not in this window
    case "approval":
      if (state) state.pendingApproval = msg;
      showApproval(msg);
      renderHome();
      break;
    case "firewall":
      if (state) state.system.firewall = msg.firewall;
      renderHome();
      renderFirewall();
      break;
    case "approvalEnded":
      // Not an answer: the headset stopped asking.
      if (state) state.pendingApproval = null;
      approvalFor = null;
      if ($("#approval").open) $("#approval").close();
      renderHome();
      renderStatus();
      break;
    case "vrState":
      if (state?.session) Object.assign(state.session, { vrState: msg.state, vrProducer: msg.producer, vrApp: msg.app });
      renderHome();
      renderStatus();
      break;
  }
});

// ---------------------------------------------------------------- navigation
function documentTitle(page, heading) {
  return page === "home" ? t("app.title") : `${heading} - ${t("app.title")}`;
}
function goTo(page, moveFocus) {
  $$("#nav button").forEach((x) => {
    if (x.dataset.page === page) x.setAttribute("aria-current", "page");
    else x.removeAttribute("aria-current");
  });
  $$(".page").forEach((p) => p.classList.toggle("active", p.dataset.page === page));
  const active = $(".page.active");
  const title = $("h1", active);
  document.title = documentTitle(page, title.textContent);
  $("#main").scrollTop = 0;
  if (moveFocus) title.focus();
  if (page !== "home") request({ type: "getState" });
  if (state) renderStatus();
}
$$("#nav button").forEach((b) =>
  // detail is 0 for keyboard activation: then focus follows to the new page.
  b.addEventListener("click", (e) => goTo(b.dataset.page, e.detail === 0)),
);

// ---------------------------------------------------------------- settings binding
function bindSettings(evenFocused = false) {
  $$("[data-setting]").forEach((el) => {
    const v = state.settings[el.dataset.setting];
    if (el.type === "checkbox") el.checked = !!v;
    else if (el.type === "radio") el.checked = String(v) === el.value;
    else if (evenFocused || document.activeElement !== el) el.value = v ?? "";
    if (el.type === "range") updateRangeLabel(el);
  });
}
function updateRangeLabel(el) {
  const text = `${el.value} Mbps`;
  el.parentElement.querySelector("output").textContent = text;
  el.setAttribute("aria-valuetext", text);
  el.style.setProperty("--fill", `${((el.value - el.min) / (el.max - el.min)) * 100}%`);
}
// A setting that failed says so next to itself. One that worked just stays as you left it.
function settingError(el, text) {
  const host = el.closest(".check, .bar, .options") || el;
  let note = host.nextElementSibling;
  if (!note?.classList.contains("failed")) {
    if (!text) return;
    note = document.createElement("p");
    note.className = "failed";
    note.setAttribute("role", "alert");
    host.after(note);
  }
  note.textContent = text;
}
function commitSetting(el) {
  const key = el.dataset.setting;
  let value = el.type === "checkbox" ? el.checked : el.value;
  if (el.type === "range" || (el.type === "radio" && /^\d+$/.test(value))) value = Number(value);
  if (key === "pcName" && !String(value).trim()) {
    settingError(el, t("err.pcName"));
    el.value = state.settings.pcName;
    return;
  }
  settingError(el, "");
  request({ type: "setSettings", settings: { [key]: value } }).then((r) => {
    if (r.ok === false || !r.settings) {
      settingError(el, r.error || t("err.save"));
      if (state) bindSettings(true);  // back to what actually applies, even under the cursor
      return;
    }
    if (state) state.settings = r.settings;
    renderDependents();
    renderAudio();
    drawSpark();
  });
}
function wireSetting(el) {
  if (el.type === "range") el.addEventListener("input", () => updateRangeLabel(el));
  el.addEventListener("change", async () => {
    if (el.dataset.setting === "requireApproval" && !el.checked &&
        !(await confirmAction(t("confirm.stopAsking.title"), t("confirm.stopAsking.text"), t("confirm.stopAsking.action")))) {
      el.checked = true;
      return;
    }
    commitSetting(el);
  });
}
$$("[data-setting]").forEach(wireSetting);

// The screenshots folder is picked in a real Windows dialog, not typed by hand.
$("#screenshotBrowse").onclick = async () => {
  const input = $("#screenshotDir");
  const r = await request({ type: "pickFolder", start: input.value });
  if (!r?.ok || !r.path) return;  // cancelled, or the design-preview mock has no dialog
  input.value = r.path;
  commitSetting(input);
};

// Opens whichever folder is configured, or the default one when the field is empty.
$("#openShotsFolder").onclick = () => send({ type: "openScreenshots" });

// ---------------------------------------------------------------- language
const languageSelect = $("#language");
function fillLanguageOptions() {
  // Option names stay in their own language: find yours even in a UI you can't read.
  const detected = LANGUAGES.find((l) => l.code === detectLanguage())?.name || detectLanguage();
  languageSelect.replaceChildren(
    new Option(t("general.languageAuto", { language: detected }), "auto"),
    ...LANGUAGES.map((l) => new Option(l.name, l.code)),
  );
  languageSelect.value = getLanguageChoice();
}
// Clearing the render caches makes every re-render pick up the new strings.
function refreshForLanguage() {
  $("#homeActions").dataset.key = "";
  $("#devices").dataset.key = "";
  $("#issues").dataset.key = "";
  accountKey = "";
  renderHome.announced = "";
  fillLanguageOptions();
  if (state) renderAll();
  const active = $(".page.active");
  document.title = documentTitle(active.dataset.page, $("h1", active).textContent);
}
languageSelect.addEventListener("change", () => {
  setLanguage(languageSelect.value);
  applyI18n();
  refreshForLanguage();
});

// ---------------------------------------------------------------- health: one truth for the home screen
const ICONS = {
  warn: '<svg viewBox="0 0 24 24" aria-hidden="true"><path d="M12 4l9 16H3zM12 10v4M12 17v.5"/></svg>',
  info: '<svg viewBox="0 0 24 24" aria-hidden="true"><circle cx="12" cy="12" r="9"/><path d="M12 11v5M12 8v.5"/></svg>',
};

// A number only counts as poor after a few seconds in a row, not on one bad sample, so nothing flickers.
const poorRuns = {};
function sustained(name, poor) {
  poorRuns[name] = poor ? (poorRuns[name] || 0) + 1 : 0;
  return poorRuns[name] >= 5;
}
function streamTrouble(s) {
  if (!numberCells(s).some((n) => n[3])) return null;
  // A slow picture with a busy encoder is the PC's doing; the rest is the network's.
  const { encodeMs } = liveNumbers(s);
  if (poorRuns.fps >= 5 && encodeMs >= 14) return { level: "warn", title: t("trouble.pc.title"), text: t("trouble.pc.text") };
  if (s.vpn) return { level: "warn", title: t("trouble.vpn.title"), text: t("trouble.vpn.text") };
  if (s.remote) return { level: "warn", title: t("trouble.remote.title"), text: t("trouble.remote.text") };
  return { level: "warn", title: t("trouble.remote.title"), text: t("trouble.network.text") };
}

function health() {
  const sys = state.system;
  const issues = [];
  if (!sys.encoder.backend || !(sys.encoder.hevc || sys.encoder.av1 || sys.encoder.h264)) {
    issues.push({ level: "blocked", title: t("issue.encoder.title"), text: t("issue.encoder.text") });
  }
  if (!sys.steamvr.installed) {
    issues.push({ level: "info", title: t("issue.steamvrMissing.title"), text: t("issue.steamvrMissing.text") });
  } else if (!sys.steamvr.driverRegistered) {
    issues.push({ level: "warn", title: t("issue.driver.title"), text: t("issue.driver.text"), action: t("vr.register"), run: registerDriver });
  }
  const a = sys.audioDriver;
  if (state.settings.streamAudio && state.settings.micPassthrough && !a.active?.speaker && !a.active?.microphone && state.settings.audioDevices !== "system") {
    issues.push({ level: "warn", title: t("issue.audio.title"), text: t("issue.audio.text") });
  }
  const c = state.cloud;
  if (c.signedIn && state.settings.allowRemote && c.status === "error") {
    issues.push({ level: "warn", title: t("issue.cloud.title"), text: t("issue.cloud.text", { error: c.error || t("err.cloudFailed") }) });
  }
  const fw = sys.firewall || {};
  if (fw.otherNetworksOpen === false) {
    issues.push({ level: "warn", title: t("issue.firewall.title"), text: t("issue.firewall.text"), action: t("issue.firewall.action"), run: () => setFirewall(!!fw.publicAllowed) });
  }
  // Not a fault: Public stays closed until someone chooses otherwise.
  if (fw.usbAdapter && fw.usbOpen === false) {
    issues.push({ level: "warn", title: t("issue.usb.title"), text: t("issue.usb.text", { adapter: fw.usbAdapter }), action: t("issue.usb.action"), run: allowUsbCable });
  }
  if (fw.publicInUse && !fw.publicAllowed) {
    issues.push({
      level: "info",
      title: t("issue.public.title"),
      text: t("issue.public.text", { vpn: (sys.vpnAddresses || []).length ? t("issue.public.vpn") : "" }),
      action: t("misc.allowOnPublic"),
      run: () => choosePublicNetworks(true),
    });
  }
  const trouble = state.session && streamTrouble(state.session);
  if (trouble) issues.push(trouble);
  const rank = { blocked: 0, warn: 1, info: 2 };
  issues.sort((a, b) => rank[a.level] - rank[b.level]);
  return { issues, blocked: issues.some((i) => i.level === "blocked") };
}

// ---------------------------------------------------------------- home
// Remembered here: "Allow once" never adds a headset to the settings.
function connectedBefore() {
  if (state.session || (state.settings.trustedDevices || []).length) {
    try { localStorage.setItem("connectedBefore", "1"); } catch {}
    return true;
  }
  try { return localStorage.getItem("connectedBefore") === "1"; } catch { return false; }
}

function setFacts(list, rows) {
  // Rewritten only when something changed, so text stays selectable while stats tick.
  const html = rows.filter((r) => r && r[1]).map(([k, v, cls]) => `<dt>${esc(k)}</dt><dd${cls ? ` class="${cls}"` : ""}>${esc(v)}</dd>`).join("");
  if (list.dataset.html !== html) {
    list.dataset.html = html;
    list.innerHTML = html;
  }
}

function encoderText() {
  const enc = state.system.encoder;
  if (!enc.backend) return t("encoder.none");
  const backend = { nvenc: "NVIDIA", amf: "AMD", mf: t("encoder.mf") }[enc.backend] || enc.backend;
  return `${backend}: ${[enc.h264 && "H.264", enc.hevc && "HEVC", enc.av1 && "AV1"].filter(Boolean).join(t("misc.listSep"))}`;
}

function renderHome() {
  if (!state) return;
  const s = state.session;
  const approval = state.pendingApproval;
  const { issues, blocked } = health();
  $("#live").hidden = true;
  $("#numbers").hidden = true;
  $("#steps").hidden = true;
  if (approval) {
    $("#headline").textContent = t("home.approvalHeadline", { name: approval.deviceName });
    $("#subline").dataset.state = "wait";
    $("#subline").textContent = t("home.approvalSub", { address: approval.address });
    setActions("", () => []);
    setFacts($("#facts"), []);
  } else if (s) {
    const inVr = s.vrState === "running";
    const app = s.vrApp || (s.vrProducer === "steamvr" ? "SteamVR" : t("home.aVrGame"));
    const streaming = inVr || (s.desktop || []).length > 0;
    $("#headline").textContent = inVr ? t("home.playing", { app }) : streaming ? t("home.streamingDesktop") : t("home.connectedTo", { name: s.deviceName });
    $("#subline").dataset.state = s.vrState === "launching" ? "wait" : "plain";
    $("#subline").textContent = s.vrState === "launching" ? t("home.startingSteamvr") : `${s.deviceName}, ${duration(s.connectedSeconds)}`;
    setActions(`session:${getLanguage()}:${s.deviceName}`, () => [button(t("home.disconnect"), "", async () => {
      const running = state.session?.vrState === "running";
      if (await confirmAction(
        t("confirm.disconnect.title", { name: s.deviceName }),
        running ? t("confirm.disconnect.vrText") : t("confirm.disconnect.plainText"),
        t("confirm.disconnect.action"),
        false,
      )) send({ type: "disconnect" });
    })]);
    $("#live").hidden = false;
    renderLive(s);
  } else {
    $("#headline").textContent = blocked ? t("home.blocked") : t("home.choosePc", { name: state.settings.pcName });
    const firstRun = !blocked && !connectedBefore();
    // The steps below say it all the first time; after that, the next step is the headline.
    if (firstRun) $("#headline").textContent = t("home.setupQuest");
    $("#subline").textContent = blocked ? t("home.blockedSub") : "";
    $("#stepPcName").textContent = state.settings.pcName;
    $("#subline").dataset.state = blocked ? "bad" : "";
    $("#steps").hidden = !firstRun;
    setActions("", () => []);
    history.length = 0;
    const devices = (state.settings.trustedDevices || []).slice().sort((a, b) => (b.lastSeen || 0) - (a.lastSeen || 0));
    const last = devices[0];
    setFacts($("#facts"), [
      [(state.system.addresses || []).length > 1 ? t("home.addresses") : t("home.address"), (state.system.addresses || []).join(", ") || t("home.noNetwork")],
      ...(state.system.vpnAddresses || []).map((v) => [v.name, t("home.vpnAddress", { address: v.address })]),
      state.cloud.signedIn && state.settings.allowRemote && state.cloud.status === "online" && [t("home.otherNetworks"), t("home.otherNetworksHint")],
      last && [t("home.lastHeadset"), `${last.name}${last.lastSeen ? `, ${new Date(last.lastSeen * 1000).toLocaleDateString(locale())}` : ""}`],
    ]);
  }
  renderIssues(issues);
  const headline = $("#headline").textContent;
  if (renderHome.announced !== headline) {
    renderHome.announced = headline;
    $("#liveStatus").textContent = headline;
  }
}

// Rebuilt only when the buttons change: stats arrive every second, and rebuilding would
// lose keyboard focus and clicks in progress.
function setActions(key, make) {
  const actions = $("#homeActions");
  if (actions.dataset.key === key) return;
  actions.dataset.key = key;
  actions.replaceChildren(...make());
}

function renderIssues(issues) {
  const list = $("#issues");
  // Rebuilt only when something changed: stats arrive every second, and the buttons must keep focus.
  const key = JSON.stringify(issues.map((i) => [i.level, i.title, i.text]));
  if (list.dataset.key === key) return;
  list.dataset.key = key;
  list.replaceChildren(...issues.map((i) => {
    const li = document.createElement("li");
    li.className = i.level;
    li.innerHTML = `<div>${i.level === "info" ? ICONS.info : ICONS.warn}<b>${esc(i.title)}</b><p>${esc(i.text)}</p></div>`;
    if (i.action) li.append(button(i.action, "", i.run));
    return li;
  }));
}

// What's on screen in the status bar and on the home page, from one place.
function liveNumbers(s) {
  const vr = s.vr && s.vrState === "running" ? s.vr : null;
  const desk = (s.desktop || []).reduce((a, d) => ({ fps: Math.max(a.fps, d.fps), mbps: a.mbps + d.mbps, encodeMs: Math.max(a.encodeMs, d.encodeMs || 0) }), { fps: 0, mbps: 0, encodeMs: 0 });
  return { vr, fps: vr ? vr.fps : desk.fps, mbps: vr ? vr.mbps : desk.mbps, encodeMs: vr ? vr.encodeMs : desk.encodeMs, latency: vr?.latencyMs || s.client?.latencyMs || s.rttMs };
}

function countPoor(s) {
  const { vr, fps, mbps, latency } = liveNumbers(s);
  const wanted = vr?.refreshRate;
  sustained("latency", latency >= 80);
  sustained("fps", !!(wanted && fps && fps < wanted * 0.95));
  sustained("mbps", !!(vr && mbps && mbps < 10));
}

function numberCells(s) {
  const { vr, fps, mbps, latency } = liveNumbers(s);
  const wanted = vr?.refreshRate;
  const halfRate = wanted && fps >= wanted * 0.45 && fps <= wanted * 0.55;
  return [
    [t("stat.latency"), latency ? Math.round(latency) : "–", latency ? (latency >= 130 ? t("unit.msHigh") : latency >= 80 ? t("unit.msNoticeable") : t("unit.ms")) : "", poorRuns.latency >= 5],
    [t("stat.fps"), fps ? Math.round(fps) : "–", fps ? (wanted && fps < wanted * 0.95 ? (halfRate ? t("unit.fpsHalf") : t("unit.fpsBelow", { rate: Math.round(wanted) })) : t("unit.fps")) : "", poorRuns.fps >= 5],
    [t("stat.bitrate"), mbps ? mbps.toFixed(mbps < 10 ? 1 : 0) : "–", mbps ? (mbps < 10 ? t("unit.mbpsLow") : t("unit.mbps")) : "", poorRuns.mbps >= 5],
  ];
}

function renderLive(s) {
  const { vr, mbps, encodeMs } = liveNumbers(s);
  const numbers = $("#numbers");
  numbers.hidden = false;
  numbers.innerHTML = numberCells(s).map(([label, value, unit, poor]) => `<div><span>${esc(label)}</span><b class="${poor ? "poor" : ""}">${esc(value)}<small>${esc(unit)}</small></b></div>`).join("");
  setFacts($("#facts"), [
    vr && [t("home.video"), t("home.videoVr", { codec: vr.codec, w: vr.eyeWidth, h: vr.eyeHeight, hz: Math.round(vr.refreshRate) })],
    !vr && (s.desktop || []).length && [t("home.video"), s.desktop.map((d) => d.name).join(", ")],
    !vr && !(s.desktop || []).length && [t("home.video"), t("home.videoNothing")],
    encodeMs && [t("home.encoding"), encodeMs < 14 ? `${encodeMs.toFixed(1)} ms` : t("home.gpuBusy", { ms: encodeMs.toFixed(1) }), encodeMs < 14 ? "" : "no"],
    [t("home.network"), s.usb ? t("home.netUsb", { rtt: s.rttMs.toFixed(1) }) : s.vpn ? t("home.netVpn", { rtt: s.rttMs.toFixed(1) }) : s.remote ? t("home.netRemote", { rtt: s.rttMs.toFixed(1) }) : t("home.netLocal", { rtt: s.rttMs.toFixed(1) })],
  ].filter(Boolean));
  renderStages(s);
  history.push(mbps || 0);
  if (history.length > 60) history.shift();
  drawSpark();
}

// The headset times each stage a frame goes through; together they make up the latency.
// The wait for the next screen refresh is left out: it's the display's own cadence,
// nothing here acts on it.
const STAGES = [
  ["uplink", "stage.uplink"],
  ["game", "stage.game"],
  ["encoderWait", "stage.encoderWait"],
  ["encode", "stage.encode"],
  ["capture", "stage.capture"],
  ["downlink", "stage.downlink"],
  ["decode", "stage.decode"],
  ["nextFrame", "stage.nextFrame"],
];

function renderStages(s) {
  const got = s.client?.latencyStages || {};
  // Stages under half a millisecond stay hidden: a clock a little off pushes some of
  // them negative, and they read as zeros.
  const rows = STAGES.filter(([key]) => typeof got[key] === "number" && got[key] >= 0.5);
  $("#stagesBox").hidden = !rows.length;
  if (!rows.length) return;
  const total = rows.reduce((sum, [key]) => sum + Math.max(got[key], 0), 0) || 1;
  const biggest = Math.max(...rows.map(([key]) => got[key]));
  $("#stages").innerHTML = rows.map(([key, label]) => {
    const ms = Math.max(got[key], 0);
    return `<div class="stage${got[key] === biggest ? " big" : ""}"><span>${esc(t(label))}</span><i style="width:${((ms / total) * 100).toFixed(1)}%"></i><b>${ms < 10 ? ms.toFixed(1) : Math.round(ms)} ms</b></div>`;
  }).join("");
}

// Live numbers while connected, for the pages that don't have them already.
function renderStatus() {
  if (!state) return;  // pushes can arrive before the first state reply
  const el = $("#status");
  const s = state.session;
  const onHome = $("#nav [aria-current]")?.dataset.page === "home";
  let html = "";
  if (s && !onHome) {
    const cell = (label, value, poor) => `<div><span>${esc(label)}</span><b class="${poor ? "poor" : ""}">${esc(value)}</b></div>`;
    html = cell(t("stat.connectedTo"), s.deviceName) + '<i class="gap"></i>' +
      numberCells(s).map(([label, value, unit, poor]) => cell(label, `${value} ${unit}`.trim(), poor)).join("");
  }
  if (el.dataset.html !== html) {
    el.dataset.html = html;
    el.innerHTML = html;
  }
}

function drawSpark() {
  const c = $("#spark");
  if (!c.clientWidth || !state) return;
  const dpr = devicePixelRatio || 1;
  const h = c.clientHeight;
  c.width = c.clientWidth * dpr;
  c.height = h * dpr;
  const g = c.getContext("2d");
  g.scale(dpr, dpr);
  const s = state.session;
  const limit = s?.vr ? s.headsetBitrateMbps || state.settings.vrBitrateMbps : state.settings.desktopBitrateMbps;
  const peak = Math.max(...history, 0);
  const w = c.clientWidth, max = Math.max(limit, peak);
  $("#sparkRange").textContent = t("home.sparkLimit", { mbps: limit });
  c.setAttribute("aria-label", t("home.sparkAria", { peak: Math.round(peak), limit }));
  const forced = matchMedia("(forced-colors: active)").matches;
  // The limit, as the top of the chart.
  g.fillStyle = forced ? "CanvasText" : "#555";
  g.fillRect(0, 0, w, 1);
  if (history.length < 2) return;
  const step = w / 59;
  const pts = history.map((v, i) => [w - (history.length - 1 - i) * step, h - (v / max) * (h - 2)]);
  g.beginPath();
  pts.forEach(([x, y], i) => (i ? g.lineTo(x, y) : g.moveTo(x, y)));
  g.strokeStyle = forced ? "CanvasText" : "#2ec4ff";
  g.lineWidth = 2;
  g.lineJoin = "round";
  g.stroke();
}

function duration(sec) {
  if (sec < 60) return t("duration.now");
  if (sec < 3600) return t("duration.min", { min: Math.floor(sec / 60) });
  return t("duration.hm", { h: Math.floor(sec / 3600), m: Math.floor((sec % 3600) / 60) });
}

function button(label, kind, onClick) {
  const b = document.createElement("button");
  b.className = `btn ${kind}`;
  b.textContent = label;
  b.onclick = onClick;
  return b;
}

// ---------------------------------------------------------------- approval
// Three explicit answers. Anything else (Esc, closing) means no.
const dialog = $("#approval");
let approvalFor = null;
function showApproval(a) {
  approvalFor = a;
  $("#approvalTitle").textContent = t("home.allowDevice", { name: a.deviceName });
  $("#approvalText").textContent = t("home.approvalText", { model: a.model || t("home.aHeadset"), address: a.address });
  if (dialog.open) return;  // state updates must not move focus
  dialog.returnValue = "";
  dialog.showModal();
  $("#denyBtn").focus();
}
function answerApproval(answer) { dialog.close(answer); }
$("#denyBtn").onclick = () => answerApproval("deny");
$("#allowOnceBtn").onclick = () => answerApproval("once");
$("#allowBtn").onclick = () => answerApproval("always");
dialog.addEventListener("close", () => {
  if (!approvalFor) return;
  const answer = dialog.returnValue;
  const allow = answer === "once" || answer === "always";
  send({ type: allow ? "approve" : "deny", deviceId: approvalFor.deviceId, remember: answer === "always" });
  if (state) state.pendingApproval = null;
  approvalFor = null;
  renderHome();
});

// ---------------------------------------------------------------- other pages
const AUDIO_SETS = {
  rincast: { speaker: "RinCast Speaker", mic: "RinCast Microphone" },
  steam: { speaker: "Steam Streaming Speakers", mic: "Steam Streaming Microphone" },
};

function setOff(row, off) {
  row.classList.toggle("off", off);
  row.querySelector("input").disabled = off;
}
function renderDependents() {
  const enc = state.system.encoder;
  const offers = { auto: true, hevc: enc.hevc, av1: enc.av1, h264: enc.h264 };
  const unsupported = [];
  $$("#codecOptions label").forEach((l) => {
    const off = !offers[l.querySelector("input").value] && !!enc.backend;
    setOff(l, off);
    if (off) unsupported.push(l.textContent.trim());
  });
  const a = state.system.audioDriver;
  $("#audioRincast").hidden = !(a.speaker || a.microphone);
  setOff($("#audioSteam"), !(a.steam?.speaker || a.steam?.microphone));
  // What the connected headset chose goes first, so here it's only for show.
  const session = state.session;
  const fromHeadset = session?.headsetBitrateMbps > 0;
  setOff($("#vrBitrateBar"), !!fromHeadset);
  $("#vrBitrateBar output").textContent = fromHeadset ? `${session.headsetBitrateMbps} Mbps` : `${state.settings.vrBitrateMbps} Mbps`;
  if (fromHeadset) $("#vrBitrateBar input").style.setProperty("--fill", `${((session.headsetBitrateMbps - 20) / 480) * 100}%`);
  const codec = session?.headsetCodec && session.headsetCodec !== "auto" ? session.headsetCodec : "";
  const chosen = [fromHeadset && t("video.overrideBitrate", { mbps: session.headsetBitrateMbps }), codec && { hevc: "HEVC", av1: "AV1", h264: "H.264" }[codec]].filter(Boolean);
  $("#overrideNote").textContent = chosen.length
    ? t("video.override", { list: chosen.join(t("misc.listSep")) })
    : t("video.overrideDefault");
  if (unsupported.length) $("#overrideNote").textContent += ` ${t("video.unsupported", { list: unsupported.join(t("misc.listSep")) })}`;
  const h264 = state.settings.codec === "h264";
  setOff($("#tenBitRow"), h264);
  $("#tenBitHint").hidden = !h264;
  setOff($("#foveatedRow"), state.system.encoder.backend !== "nvenc");
  const tunable = ["nvenc", "amf"].includes(state.system.encoder.backend);
  setOff($("#aqRow"), !tunable);
  setOff($("#twoPassRow"), !tunable);
  setOff($("#playOnPcRow"), !state.settings.streamAudio);
  setOff($("#micRow"), state.settings.audioDevices === "system");
}

let installingSteamAudio = false;

$("#installSteamAudio").onclick = async () => {
  if (installingSteamAudio) return;
  installingSteamAudio = true;
  renderAudio();
  $("#audioInstallStatus").textContent = t("audio.installing");
  try {
    const r = await request({ type: "installSteamAudio" }, 90000);
    $("#audioInstallStatus").textContent = r.ok ? t("audio.installDone") :
      (r.error?.startsWith("audio.install") ? t(r.error, { code: r.code }) : r.error || t("audio.installFailed", { code: r.code }));
    await request({ type: "getState" });
  } finally {
    installingSteamAudio = false;
    renderAudio();
  }
};

function renderAudio() {
  if (!state) return;
  const a = state.system.audioDriver;
  const steam = a.steam || {};
  $("#installSteamAudio").disabled = installingSteamAudio || (steam.speaker && steam.microphone);
  const active = a.active || {};
  const row = (ok, name, inUse) => [name, ok ? (inUse ? t("audio.inUse") : t("audio.installed")) : t("audio.notInstalled"), ok ? "" : "no"];
  const rows = [
    row(steam.speaker, AUDIO_SETS.steam.speaker, active.speaker === "steam"),
    row(steam.microphone, AUDIO_SETS.steam.mic, active.microphone === "steam"),
  ];
  // The RinCast driver only ships with signed builds: don't list it as missing otherwise.
  if (a.speaker || a.microphone) {
    rows.unshift(row(a.microphone && a.microphoneFeed, AUDIO_SETS.rincast.mic, active.microphone === "rincast"));
    rows.unshift(row(a.speaker, AUDIO_SETS.rincast.speaker, active.speaker === "rincast"));
  }
  // Only what's missing needs saying.
  const missing = rows.filter((r) => r[2] === "no").map((r) => r[0]);
  $("#audioMissing").textContent = missing.length
    ? t(missing.length > 1 ? "audio.missingMany" : "audio.missingOne", { names: missing.join(t("misc.and")) })
    : "";
  const mic = AUDIO_SETS[active.microphone]?.mic;
  $("#micHint").textContent = mic ? t("audio.micHint", { mic }) : t("audio.micHintNone");
  const speaker = AUDIO_SETS[active.speaker]?.speaker;
  if (!speaker) $("#audioDevicesHint").textContent = t("audio.usingDefault");
  else {
    const list = mic && active.microphone !== active.speaker ? [speaker, mic].join(t("misc.and")) : speaker;
    $("#audioDevicesHint").textContent = t("audio.using", { list });
  }
}

async function allowUsbCable() {
  toast(t("toast.acceptPrompt"));
  const r = await request({ type: "allowUsbCable" }, 90000);
  if (r.ok) toast(t("toast.usbAllowed"));
  else showError(t("err.firewall"));
  await request({ type: "getState" });
}

async function setFirewall(allowPublic) {
  toast(t("toast.acceptPrompt"));
  const r = await request({ type: "setFirewall", allowPublic }, 90000);
  if (r.ok) toast(t(allowPublic ? "toast.publicAllowed" : "toast.publicClosed"));
  else showError(t("err.firewall"));
  await request({ type: "getState" });
}

// Opening up asks first, and says what it means. Closing doesn't need convincing.
async function choosePublicNetworks(on) {
  if (on) {
    const risk = state.settings.requireApproval
      ? t("confirm.public.riskGuarded")
      : t("confirm.public.riskUnguarded");
    const yes = await confirmAction(t("confirm.public.title"), t("confirm.public.text", { risk }), t("misc.allowOnPublic"));
    if (!yes) {
      renderFirewall();
      return;
    }
  }
  await setFirewall(on);
  renderFirewall();
}

function renderFirewall() {
  const fw = state.system.firewall || {};
  const box = $("#allowPublic");
  box.checked = !!fw.publicAllowed;
  setOff($("#publicRow"), !!fw.publicUnguarded);
  $("#publicNote").textContent = fw.publicUnguarded ? t("general.publicNoteUnguarded") : t("general.publicNoteOpen");
}
$("#allowPublic").addEventListener("change", (e) => choosePublicNetworks(e.target.checked));

async function registerDriver() {
  const r = await request({ type: "registerSteamVrDriver" });
  if (r.ok) toast(t("toast.driverAdded"));
  else showError(t("err.driverRegister"));
  request({ type: "getState" });
}

function renderVr() {
  const sys = state.system;
  $$("#runtimeSeg input").forEach((b) => { b.checked = (b.value === "rincast" && sys.openxr.rincastActive) || (b.value === "steamvr" && sys.openxr.steamvrActive); });
  $("#runtimeRow").hidden = sys.openxr.rincastAvailable === false;
  $("#runtimeNote").textContent = sys.openxr.rincastActive || sys.openxr.steamvrActive ? "" : t("vr.otherRuntime");
  setFacts($("#vrFacts"), [
    [t("vr.status"), !sys.steamvr.installed ? t("vr.notInstalled") : sys.steamvr.running ? t("vr.running") : t("vr.notRunning"), sys.steamvr.installed ? "" : "no"],
    sys.steamvr.installed && [t("vr.driver"), sys.steamvr.driverRegistered ? t("vr.added") : t("vr.notAdded"), sys.steamvr.driverRegistered ? "" : "no"],
  ]);
  $("#registerDriver").hidden = !sys.steamvr.installed || sys.steamvr.driverRegistered;
  $("#launchSteamVR").hidden = !sys.steamvr.installed || sys.steamvr.running;
  $("#restartSteamVR").hidden = !sys.steamvr.running;
  $("#getSteamVR").hidden = sys.steamvr.installed;
}
$$("#runtimeSeg input").forEach((b) => b.addEventListener("change", async () => {
  toast(t("toast.acceptPrompt"));
  const r = await request({ type: "setOpenXrRuntime", runtime: b.value });
  if (r.ok) toast(t("toast.runtimeSwitch", { runtime: b.nextElementSibling.textContent }));
  else showError(t("err.runtimeSwitch"));
  request({ type: "getState" });
}));
$("#registerDriver").onclick = registerDriver;
$("#getSteamVR").onclick = () => send({ type: "openUrl", url: "https://store.steampowered.com/app/250820/SteamVR/" });
$("#launchSteamVR").onclick = () => { send({ type: "launchSteamVR" }); toast(t("toast.startingSteamvr")); };
$("#restartSteamVR").onclick = async () => {
  if (await confirmAction(t("confirm.restartSteamvr.title"), t("confirm.restartSteamvr.text"), t("confirm.restartSteamvr.action"))) { send({ type: "restartSteamVR" }); toast(t("toast.restartSteamvr")); }
};
$("#openLogs").onclick = () => send({ type: "openLogs" });

function renderDevices() {
  const list = $("#devices");
  const devices = state.settings.trustedDevices || [];
  const key = `${getLanguage()}:${JSON.stringify(devices)}`;
  if (list.dataset.key === key) return;
  list.dataset.key = key;
  if (!devices.length) {
    list.innerHTML = `<li class="empty">${esc(t("devices.empty"))}</li>`;
    return;
  }
  list.innerHTML = devices.map((d) => `
    <li><div class="meta"><b>${esc(d.name)}</b><small>${esc(t("devices.lastSeen", { date: d.lastSeen ? new Date(d.lastSeen * 1000).toLocaleDateString(locale()) : t("devices.never") }))}</small></div>
    <button class="btn" data-remove="${esc(d.id)}" data-name="${esc(d.name)}" aria-label="${esc(t("devices.removeAria", { name: d.name }))}">${esc(t("devices.remove"))}</button></li>`).join("");
  $$("[data-remove]", list).forEach((b) => (b.onclick = async () => {
    if (!(await confirmAction(t("confirm.remove.title", { name: b.dataset.name }), t("confirm.remove.text"), t("confirm.remove.action")))) return;
    const r = await request({ type: "removeTrusted", deviceId: b.dataset.remove });
    if (r.ok === false) showError(t("err.removeDevice"));
    else toast(t("toast.deviceRemoved"));
    request({ type: "getState" });
  }));
}

let authMode = "signIn";
let accountKey = "";
function renderAccount() {
  const view = $("#accountView");
  const c = state.cloud;
  const key = `${getLanguage()}:${c.signedIn ? `in:${c.userName}:${c.email}` : `out:${authMode}`}`;
  if (key !== accountKey) {
    accountKey = key;  // otherwise leave the form alone: state updates must not erase typing
    if (c.signedIn) renderSignedIn(view, c);
    else renderSignedOut(view);
  }
  if (c.signedIn) {
    const allow = $("#allowRemote");
    allow.checked = state.settings.allowRemote;
    $("#remoteStatus").textContent = !state.settings.allowRemote
      ? t("account.remoteOff")
      : { online: t("account.remoteOnline"), connecting: t("account.remoteConnecting"), error: c.error || t("account.remoteError") }[c.status] || "";
  }
}
function renderSignedIn(view, c) {
  view.innerHTML = `
    <div class="cols">
      <div>
        <h2 class="first">${esc(c.userName)}</h2>
        <p class="note selectable">${esc(c.email)}</p>
        <label class="check spaced"><input type="checkbox" data-setting="allowRemote" id="allowRemote"><span>${esc(t("account.allowRemote"))}</span><small id="remoteStatus"></small></label>
        <div class="btns spaced"><button class="btn" id="signOut">${esc(t("account.signOut"))}</button></div>
      </div>
      <div>
        <h2 class="first">${esc(t("account.signInHeadset"))}</h2>
        <h3 id="codeLabel">${esc(t("account.codeLabel"))}</h3>
        <form class="code-entry" id="codeForm"><input type="text" id="code" maxlength="9" placeholder="XXXX-XXXX" autocomplete="off" spellcheck="false" aria-labelledby="codeLabel"><button class="btn primary" id="codeSubmit">${esc(t("account.approve"))}</button></form>
        <p class="failed" id="codeError" role="alert"></p>
      </div>
    </div>`;
  wireSetting($("#allowRemote"));
  $("#codeForm").onsubmit = async (e) => {
    e.preventDefault();
    $("#codeError").textContent = "";
    const submit = $("#codeSubmit");
    submit.disabled = true;
    submit.textContent = t("account.approving");
    const r = await request({ type: "approveCode", code: $("#code").value });
    submit.disabled = false;
    submit.textContent = t("account.approve");
    if (r.ok) { toast(t("toast.headsetSignedIn")); $("#code").value = ""; }
    else $("#codeError").textContent = r.error || t("err.code");
  };
  $("#signOut").onclick = async () => {
    if (!(await confirmAction(t("confirm.signOut.title"), t("confirm.signOut.text"), t("confirm.signOut.action")))) return;
    const r = await request({ type: "signOut" });
    if (r.ok === false) showError(r.error || t("err.signOut"));
    else toast(t("toast.signedOut"));
    request({ type: "getState" });
  };
}
function renderSignedOut(view) {
  const register = authMode === "register";
  view.innerHTML = `
    <h2 class="first">${esc(register ? t("account.createTitle") : t("account.signInTitle"))}</h2>
    <p class="note auth-note">${esc(t("account.authNote"))}</p>
    <form class="auth" id="authForm" novalidate>
      ${register ? `<label for="authName">${esc(t("account.name"))}</label><input type="text" id="authName" name="name" autocomplete="nickname">` : ""}
      <label for="authEmail">${esc(t("account.email"))}</label>
      <input type="email" id="authEmail" name="email" autocomplete="email" required aria-describedby="authError">
      <label for="authPassword">${esc(t("account.password"))}${register ? ` ${esc(t("account.passwordHint"))}` : ""}</label>
      <input type="password" id="authPassword" name="password" autocomplete="${register ? "new-password" : "current-password"}" required aria-describedby="authError">
      <p class="error" id="authError" role="alert"></p>
      <button class="btn primary" id="authSubmit">${esc(register ? t("account.createSubmit") : t("account.signInSubmit"))}</button>
      <p class="switch-mode">${register
        ? `${esc(t("account.haveAccount"))} <button type="button" class="link" id="toggleAuth">${esc(t("account.signInSubmit"))}</button>`
        : `${esc(t("account.noAccount"))} <button type="button" class="link" id="toggleAuth">${esc(t("account.createSubmit"))}</button>`}</p>
      ${register ? "" : `<p class="switch-mode">${esc(t("account.forgot"))}</p>`}
    </form>`;
  $("#toggleAuth").onclick = () => { authMode = register ? "signIn" : "register"; renderAccount(); ($("#authName") || $("#authEmail")).focus(); };
  $("#authForm").onsubmit = async (e) => {
    e.preventDefault();
    const data = Object.fromEntries(new FormData(e.target));
    const err = $("#authError");
    err.textContent = "";
    if (!/^\S+@\S+\.\S+$/.test(data.email || "")) { err.textContent = t("err.email"); $("#authEmail").focus(); return; }
    if (register && (data.password || "").length < 8) { err.textContent = t("err.password"); $("#authPassword").focus(); return; }
    const btn = $("#authSubmit");
    btn.disabled = true;
    btn.textContent = register ? t("account.creating") : t("account.signingIn");
    const r = await request({ type: authMode, ...data });
    btn.disabled = false;
    btn.textContent = register ? t("account.createSubmit") : t("account.signInSubmit");
    if (!r.ok) { err.textContent = r.error || t("err.auth"); return; }
    toast(register ? t("toast.accountCreated") : t("toast.signedIn"));
    request({ type: "getState" });
  };
}

$("#downloadLink").onclick = () => send({ type: "openUrl", url: state.downloadUrl });

function renderRail() {
  $("#downloadLink").textContent = t("home.getIt", { site: state.downloadSite });
  setFacts($("#aboutFacts"), [
    [t("about.gpu"), state.system.gpu],
    state.system.encoder.backend && [t("about.encoder"), encoderText()],
    [t("about.version"), state.version],
  ]);
  $("#aboutLine").textContent = t("about.line", { port: state.system.port });
}

function renderAll() {
  bindSettings();
  renderHome();
  renderStatus();
  renderFirewall();
  renderDependents();
  renderAudio();
  renderVr();
  renderDevices();
  renderAccount();
  renderRail();
  if (state.pendingApproval) showApproval(state.pendingApproval);
}

window.addEventListener("resize", () => history.length && drawSpark());
fillLanguageOptions();
request({ type: "getState" });

// ---------------------------------------------------------------- design-preview mock
function mock(msg) {
  const demo = new URLSearchParams(location.search).get("demo") || "connected";
  const none = demo === "firstrun" || demo === "problem" || demo === "signedout";
  if (demo === "firstrun") localStorage.removeItem("connectedBefore");
  const audio = { speaker: false, microphone: false, microphoneFeed: false, steam: { speaker: !(demo === "problem"), microphone: !(demo === "problem") }, active: demo === "problem" ? {} : { speaker: "steam", microphone: "steam" } };
  const base = mock.base ||= {
    type: "state", version: "1.0.0", downloadUrl: "https://rincast.com/#download", downloadSite: "rincast.com",
    settings: { pcName: "Studio PC", codec: "auto", tenBit: false, vrBitrateMbps: 200, desktopBitrateMbps: 40, adaptiveBitrate: true, fecPercent: 5, foveatedEncoding: true, adaptiveQuantization: true, twoPassEncoding: false, desktopMaxHeight: 0, streamAudio: true, playOnPcToo: false, micPassthrough: true, switchDefaultAudio: true, audioDevices: "auto", autoRegisterSteamVrDriver: true, requireApproval: true, desktopInput: true, startWithWindows: true, preventSleep: true, cloudUrl: "https://rincast.com", allowRemote: true,
      trustedDevices: none ? [] : [{ id: "q3", name: "Quest 3", model: "Meta Quest 3", lastSeen: Date.now() / 1000 }] },
    cloud: demo === "signedout" || demo === "firstrun" ? { signedIn: false } : { signedIn: true, userName: "Sam", email: "sam@example.com", status: demo === "problem" ? "error" : "online", error: demo === "problem" ? "The server didn't answer." : "", allowRemote: true },
    system: { gpu: demo === "problem" ? "Microsoft Basic Display Adapter" : "NVIDIA GeForce RTX 5070 Ti", port: 38711, addresses: ["192.168.1.20"], vpnAddresses: demo === "vpn" ? [{ name: "Tailscale", address: "100.101.102.103" }] : [], firewall: { otherNetworksOpen: demo !== "problem", publicInUse: demo === "public", publicAllowed: false, publicUnguarded: false }, encoder: demo === "problem" ? {} : { backend: "nvenc", h264: true, hevc: true, av1: true, hevc10: true },
      steamvr: { installed: true, running: false, driverRegistered: demo !== "problem" }, openxr: { rincastAvailable: false, rincastActive: false, steamvrActive: true },
      audioDriver: audio },
    session: demo === "connected" || demo === "lowbandwidth" ? { deviceName: "Quest 3", model: "Meta Quest 3", address: "192.168.1.42", remote: demo === "lowbandwidth", connectedSeconds: 1460, rttMs: demo === "lowbandwidth" ? 42.6 : 3.4, vrState: "running", vrApp: "Half-Life: Alyx",
      headsetBitrateMbps: 0, headsetCodec: "auto", vr: { fps: demo === "lowbandwidth" ? 45 : 90, encodeMs: 3.1, mbps: demo === "lowbandwidth" ? 7 : 182, codec: "HEVC", eyeWidth: 1536, eyeHeight: 1632, refreshRate: 90, latencyMs: demo === "lowbandwidth" ? 96 : 38 }, desktop: [],
      client: { latencyStages: demo === "lowbandwidth" ? { uplink: 21.3, game: 12.1, encoderWait: 0.7, encode: 4.4, downlink: 41.8, decode: 4.6, nextFrame: 3.1, compositor: 8 } : { uplink: 1.7, game: 11.4, encoderWait: 0.6, encode: 4.1, downlink: 5.3, decode: 4.2, nextFrame: 2.9, compositor: 7.8 } } } : null,
    screenshots: [{ name: "Screenshot_2026.11.11-12.00.00.jpg", path: "" }, { name: "Screenshot_2026.11.11-11.59.30.jpg", path: "" }],
    pendingApproval: demo === "approval" ? { deviceId: "x", deviceName: "Quest 3", model: "Meta Quest 3", address: "192.168.1.42" } : null,
  };
  setTimeout(() => {
    if (msg.type === "getState") receive({ ...base, requestId: msg.requestId });
    else if (msg.type === "setSettings") { Object.assign(base.settings, msg.settings); receive({ type: "reply", requestId: msg.requestId, ok: true, settings: base.settings }); }
    else if (msg.requestId) receive({ type: "reply", requestId: msg.requestId, ok: true, settings: base.settings });
  }, 30);
  if (!mock.timer && base.session) {
    for (let i = 0; i < 40; i++) history.push(base.session.vr.mbps * (0.86 + Math.random() * 0.14));
    mock.timer = setInterval(() => {
      if (demo === "connected") {
        base.session.vr.mbps = 170 + Math.random() * 25;
        base.session.vr.latencyMs = 34 + Math.random() * 8;
      }
      receive({ type: "stats", session: base.session });
    }, 1000);
  }
}
