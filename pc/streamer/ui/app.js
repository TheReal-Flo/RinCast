// RinCast Streamer UI. Talks to the native app through WebView2 messaging;
// outside WebView2 (design preview) a small mock backend is used.

const $ = (s, r = document) => r.querySelector(s);
const $$ = (s, r = document) => [...r.querySelectorAll(s)];
const esc = (s) => String(s ?? "").replace(/[&<>"']/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));

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
      resolve({ ok: false, error: "RinCast didn't respond. Try again." });
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
  const t = $("#toast");
  t.textContent = text;
  t.classList.add("show");
  clearTimeout(toast.timer);
  toast.timer = setTimeout(() => t.classList.remove("show"), 2600);
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
      if (msg.connected) toast(`${msg.deviceName} connected`);
      else if (msg.reason !== "replaced") toast(`${msg.deviceName} disconnected`);
      break;
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
function goTo(page, moveFocus) {
  $$("#nav button").forEach((x) => {
    if (x.dataset.page === page) x.setAttribute("aria-current", "page");
    else x.removeAttribute("aria-current");
  });
  $$(".page").forEach((p) => p.classList.toggle("active", p.dataset.page === page));
  const active = $(".page.active");
  const title = $("h1", active);
  document.title = page === "home" ? "RinCast Streamer" : `${title.textContent} - RinCast Streamer`;
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
function bindSettings() {
  $$("[data-setting]").forEach((el) => {
    const v = state.settings[el.dataset.setting];
    if (el.type === "checkbox") el.checked = !!v;
    else if (el.type === "radio") el.checked = String(v) === el.value;
    else if (document.activeElement !== el) el.value = v ?? "";
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
    settingError(el, "The PC needs a name.");
    el.value = state.settings.pcName;
    return;
  }
  settingError(el, "");
  request({ type: "setSettings", settings: { [key]: value } }).then((r) => {
    if (r.ok === false || !r.settings) {
      settingError(el, r.error || "Couldn't save this. Try again.");
      if (state) bindSettings();  // back to what actually applies
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
        !(await confirmAction("Stop asking?", "Any headset that can reach this PC could then see your screen and control it, without you noticing. If you're signed in, that includes anyone with your account password.", "Stop asking"))) {
      el.checked = true;
      return;
    }
    commitSetting(el);
  });
}
$$("[data-setting]").forEach(wireSetting);

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
  if (poorRuns.fps >= 5 && encodeMs >= 14) return { level: "warn", title: "This PC is at its limit", text: "Lower the VR graphics quality on the headset, or close other programs." };
  if (s.vpn) return { level: "warn", title: "The VPN is limiting the stream", text: "A VPN that relays its traffic is slow. A direct path between the two devices helps." };
  if (s.remote) return { level: "warn", title: "The connection is limiting the stream", text: "This is usual over the internet. A faster connection on either side helps." };
  return { level: "warn", title: "The connection is limiting the stream", text: "A cable from the PC to the router and staying near the router help most." };
}

function health() {
  const sys = state.system;
  const issues = [];
  if (!sys.encoder.backend || !(sys.encoder.hevc || sys.encoder.av1 || sys.encoder.h264)) {
    issues.push({ level: "blocked", title: "This PC can't encode video", text: "RinCast needs the video encoder of an NVIDIA, AMD or Intel graphics card. Update your graphics driver, then restart RinCast." });
  }
  if (!sys.steamvr.installed) {
    issues.push({ level: "info", title: "SteamVR isn't installed", text: "You need it for VR games, not for the desktop. Get it from Steam." });
  } else if (!sys.steamvr.driverRegistered) {
    issues.push({ level: "warn", title: "RinCast isn't in SteamVR yet", text: "VR games won't reach your headset until it is.", action: "Add RinCast to SteamVR", run: registerDriver });
  }
  const a = sys.audioDriver;
  if (state.settings.streamAudio && state.settings.micPassthrough && !a.active?.speaker && !a.active?.microphone && state.settings.audioDevices !== "system") {
    issues.push({ level: "warn", title: "No virtual audio devices", text: "Your headset microphone won't reach apps. Turn on Remote Play in Steam to add them." });
  }
  const c = state.cloud;
  if (c.signedIn && state.settings.allowRemote && c.status === "error") {
    issues.push({ level: "warn", title: "Can't reach the RinCast server", text: `${c.error || "The connection failed."} Your own network still works.` });
  }
  const fw = sys.firewall || {};
  if (fw.otherNetworksOpen === false) {
    issues.push({ level: "warn", title: "Windows' firewall is blocking headsets", text: "RinCast has to be allowed through it. Windows asks for your permission.", action: "Allow RinCast", run: () => setFirewall(!!fw.publicAllowed) });
  }
  // Not a fault: Public stays closed until someone chooses otherwise.
  if (fw.usbAdapter && fw.usbOpen === false) {
    issues.push({ level: "warn", title: "Windows' firewall is blocking the USB cable", text: `A headset is plugged in by cable ("${fw.usbAdapter}"). Windows treats that as an unknown network. Allowing it opens only that cable, no other network.`, action: "Allow the USB cable", run: allowUsbCable });
  }
  if (fw.publicInUse && !fw.publicAllowed) {
    issues.push({ level: "info", title: "Headsets can't connect over a network set as Public", text: `Windows has one of this PC's networks set as Public${(sys.vpnAddresses || []).length ? ", which can be a VPN" : ""}. Allow it only if you trust that network.`, action: "Allow on Public networks", run: () => choosePublicNetworks(true) });
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
  if (!enc.backend) return "None found. Update your graphics driver.";
  const backend = { nvenc: "NVIDIA", amf: "AMD", mf: "Windows Media Foundation" }[enc.backend] || enc.backend;
  return `${backend}: ${[enc.h264 && "H.264", enc.hevc && "HEVC", enc.av1 && "AV1"].filter(Boolean).join(", ")}`;
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
    $("#headline").textContent = `${approval.deviceName} wants to connect`;
    $("#subline").dataset.state = "wait";
    $("#subline").textContent = `From ${approval.address}. Only allow it if it's yours.`;
    setActions("", () => []);
    setFacts($("#facts"), []);
  } else if (s) {
    const inVr = s.vrState === "running";
    const app = s.vrApp || (s.vrProducer === "steamvr" ? "SteamVR" : "a VR game");
    const streaming = inVr || (s.desktop || []).length > 0;
    $("#headline").textContent = inVr ? `Playing ${app}` : streaming ? "Streaming your desktop" : `Connected to ${s.deviceName}`;
    $("#subline").dataset.state = s.vrState === "launching" ? "wait" : "plain";
    $("#subline").textContent = s.vrState === "launching" ? "Starting SteamVR…" : `${s.deviceName}, ${duration(s.connectedSeconds)}`;
    setActions(`session:${s.deviceName}`, () => [button("Disconnect", "", async () => {
      const running = state.session?.vrState === "running";
      if (await confirmAction(`Disconnect ${s.deviceName}?`, running ? "The game keeps running on this PC. The headset goes back to its home screen." : "The headset goes back to its home screen.", "Disconnect", false)) send({ type: "disconnect" });
    })]);
    $("#live").hidden = false;
    renderLive(s);
  } else {
    $("#headline").textContent = blocked ? "Can't stream yet" : `Choose ${state.settings.pcName} on your headset`;
    const firstRun = !blocked && !connectedBefore();
    // The steps below say it all the first time; after that, the next step is the headline.
    if (firstRun) $("#headline").textContent = "Set up your Quest";
    $("#subline").textContent = blocked ? "RinCast can't stream until the first problem below is fixed." : "";
    $("#stepPcName").textContent = state.settings.pcName;
    $("#subline").dataset.state = blocked ? "bad" : "";
    $("#steps").hidden = !firstRun;
    setActions("", () => []);
    history.length = 0;
    const devices = (state.settings.trustedDevices || []).slice().sort((a, b) => (b.lastSeen || 0) - (a.lastSeen || 0));
    const last = devices[0];
    setFacts($("#facts"), [
      [(state.system.addresses || []).length > 1 ? "Addresses" : "Address", (state.system.addresses || []).join(", ") || "No network"],
      ...(state.system.vpnAddresses || []).map((v) => [v.name, `${v.address} (VPN: enter this on the headset)`]),
      state.cloud.signedIn && state.settings.allowRemote && state.cloud.status === "online" && ["Other networks", "This PC can be reached from them"],
      last && ["Last headset", `${last.name}${last.lastSeen ? `, ${new Date(last.lastSeen * 1000).toLocaleDateString()}` : ""}`],
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
    ["Latency", latency ? Math.round(latency) : "–", latency ? (latency >= 130 ? "ms (high)" : latency >= 80 ? "ms (noticeable)" : "ms") : "", poorRuns.latency >= 5],
    ["Frame rate", fps ? Math.round(fps) : "–", fps ? (wanted && fps < wanted * 0.95 ? (halfRate ? "fps (half rate)" : `fps (below ${Math.round(wanted)})`) : "fps") : "", poorRuns.fps >= 5],
    ["Bitrate", mbps ? mbps.toFixed(mbps < 10 ? 1 : 0) : "–", mbps ? (mbps < 10 ? "Mbps (low)" : "Mbps") : "", poorRuns.mbps >= 5],
  ];
}

function renderLive(s) {
  const { vr, mbps, encodeMs } = liveNumbers(s);
  const numbers = $("#numbers");
  numbers.hidden = false;
  numbers.innerHTML = numberCells(s).map(([label, value, unit, poor]) => `<div><span>${esc(label)}</span><b class="${poor ? "poor" : ""}">${esc(value)}<small>${esc(unit)}</small></b></div>`).join("");
  setFacts($("#facts"), [
    vr && ["Video", `${vr.codec}, ${vr.eyeWidth} × ${vr.eyeHeight} per eye, ${Math.round(vr.refreshRate)} Hz`],
    !vr && (s.desktop || []).length && ["Video", s.desktop.map((d) => d.name).join(", ")],
    !vr && !(s.desktop || []).length && ["Video", "Nothing yet. Open a desktop or game in the headset."],
    encodeMs && ["Encoding", encodeMs < 14 ? `${encodeMs.toFixed(1)} ms` : `${encodeMs.toFixed(1)} ms, the GPU is busy`, encodeMs < 14 ? "" : "no"],
    ["Network", `${s.rttMs.toFixed(1)} ms round trip, ${s.usb ? "over a USB cable" : s.vpn ? "over a VPN" : s.remote ? "over the internet" : "on your network"}`],
  ].filter(Boolean));
  renderStages(s);
  history.push(mbps || 0);
  if (history.length > 60) history.shift();
  drawSpark();
}

// The headset times each stage a frame goes through; together they make up the latency.
const STAGES = [
  ["uplink", "Head movement to the PC"],
  ["game", "Game and SteamVR"],
  ["encoderWait", "Waiting for the encoder"],
  ["encode", "Encoding"],
  ["capture", "Capture and encoding"],
  ["downlink", "Video to the headset"],
  ["decode", "Decoding in the headset"],
  ["nextFrame", "Waiting for the headset's next frame"],
  ["compositor", "Headset compositor and display"],
];

function renderStages(s) {
  const got = s.client?.latencyStages || {};
  const rows = STAGES.filter(([key]) => typeof got[key] === "number");
  $("#stagesBox").hidden = !rows.length;
  if (!rows.length) return;
  const total = rows.reduce((sum, [key]) => sum + Math.max(got[key], 0), 0) || 1;
  const biggest = Math.max(...rows.map(([key]) => got[key]));
  $("#stages").innerHTML = rows.map(([key, label]) => {
    const ms = Math.max(got[key], 0);
    return `<div class="stage${got[key] === biggest ? " big" : ""}"><span>${esc(label)}</span><i style="width:${((ms / total) * 100).toFixed(1)}%"></i><b>${ms < 10 ? ms.toFixed(1) : Math.round(ms)} ms</b></div>`;
  }).join("");
}

// Live numbers while connected, for the pages that don't have them already.
function renderStatus() {
  const el = $("#status");
  const s = state.session;
  const onHome = $("#nav [aria-current]")?.dataset.page === "home";
  let html = "";
  if (s && !onHome) {
    const cell = (label, value, poor) => `<div><span>${esc(label)}</span><b class="${poor ? "poor" : ""}">${esc(value)}</b></div>`;
    html = cell("Connected to", s.deviceName) + '<i class="gap"></i>' +
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
  $("#sparkRange").textContent = `Limit ${limit} Mbps`;
  c.setAttribute("aria-label", `Bitrate over the last minute, peak ${Math.round(peak)} of ${limit} Mbps`);
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
  if (sec < 60) return "just connected";
  if (sec < 3600) return `connected for ${Math.floor(sec / 60)} min`;
  return `connected for ${Math.floor(sec / 3600)} h ${Math.floor((sec % 3600) / 60)} min`;
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
  $("#approvalTitle").textContent = `Allow ${a.deviceName}?`;
  $("#approvalText").textContent = `${a.model || "A headset"} at ${a.address} wants to stream from this PC.`;
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
  const chosen = [fromHeadset && `bitrate limit ${session.headsetBitrateMbps} Mbps`, codec && { hevc: "HEVC", av1: "AV1", h264: "H.264" }[codec]].filter(Boolean);
  $("#overrideNote").textContent = chosen.length ? `The connected headset overrides these: ${chosen.join(", ")}.` : "Bitrate and codec set on the headset override these";
  if (unsupported.length) $("#overrideNote").textContent += ` This graphics card can't encode ${unsupported.join(" or ")}.`;
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

function renderAudio() {
  if (!state) return;
  const a = state.system.audioDriver;
  const steam = a.steam || {};
  const active = a.active || {};
  const row = (ok, name, inUse) => [name, ok ? (inUse ? "Used while streaming" : "Installed") : "Not installed", ok ? "" : "no"];
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
  $("#audioMissing").textContent = missing.length ? `${missing.join(" and ")} ${missing.length > 1 ? "aren't" : "isn't"} installed. Turn on Remote Play in Steam to add ${missing.length > 1 ? "them" : "it"}.` : "";
  const mic = AUDIO_SETS[active.microphone]?.mic;
  $("#micHint").textContent = mic ? `In apps, choose ${mic} as the input` : "No virtual microphone available. Turn on Remote Play in Steam to add one";
  const speaker = AUDIO_SETS[active.speaker]?.speaker;
  $("#audioDevicesHint").textContent = speaker ? `Using ${speaker}${mic && active.microphone !== active.speaker ? ` and ${mic}` : ""}` : "PC sound comes from your default speakers";
}

async function allowUsbCable() {
  toast("Accept the Windows prompt");
  const r = await request({ type: "allowUsbCable" }, 90000);
  if (r.ok) toast("Headsets can connect over the USB cable");
  else showError("The firewall didn't change. Windows' permission prompt was declined or failed.");
  await request({ type: "getState" });
}

async function setFirewall(allowPublic) {
  toast("Accept the Windows prompt");
  const r = await request({ type: "setFirewall", allowPublic }, 90000);
  if (r.ok) toast(allowPublic ? "Headsets can connect on Public networks" : "Public networks are closed to headsets");
  else showError("The firewall didn't change. Windows' permission prompt was declined or failed.");
  await request({ type: "getState" });
}

// Opening up asks first, and says what it means. Closing doesn't need convincing.
async function choosePublicNetworks(on) {
  if (on) {
    const risk = state.settings.requireApproval
      ? "A new headset still has to be allowed on this PC."
      : "\"Ask before a new headset connects\" is off, so anyone on such a network could see your screen and control this PC. Turn it on under Headsets first.";
    const yes = await confirmAction("Allow headsets on Public networks?", `On a network set as Public, such as in a café or hotel, anyone can then see this PC's name, graphics card and RinCast account name in RinCast, and ask to connect. ${risk} Windows asks for your permission next.`, "Allow on Public networks");
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
  $("#publicNote").textContent = fw.publicUnguarded ? "Windows' firewall is off on Public networks" : "Anyone on those networks can then see this PC and ask to connect";
}
$("#allowPublic").addEventListener("change", (e) => choosePublicNetworks(e.target.checked));

async function registerDriver() {
  const r = await request({ type: "registerSteamVrDriver" });
  if (r.ok) toast("RinCast is in SteamVR");
  else showError("Couldn't add RinCast to SteamVR. Start SteamVR once, then try again.");
  request({ type: "getState" });
}

function renderVr() {
  const sys = state.system;
  $$("#runtimeSeg input").forEach((b) => { b.checked = (b.value === "rincast" && sys.openxr.rincastActive) || (b.value === "steamvr" && sys.openxr.steamvrActive); });
  $("#runtimeRow").hidden = sys.openxr.rincastAvailable === false;
  $("#runtimeNote").textContent = sys.openxr.rincastActive || sys.openxr.steamvrActive ? "" : "Another app handles OpenXR games right now.";
  setFacts($("#vrFacts"), [
    ["Status", !sys.steamvr.installed ? "Not installed" : sys.steamvr.running ? "Running" : "Not running", sys.steamvr.installed ? "" : "no"],
    sys.steamvr.installed && ["RinCast driver", sys.steamvr.driverRegistered ? "Added" : "Not added yet", sys.steamvr.driverRegistered ? "" : "no"],
  ]);
  $("#registerDriver").hidden = !sys.steamvr.installed || sys.steamvr.driverRegistered;
  $("#launchSteamVR").hidden = !sys.steamvr.installed || sys.steamvr.running;
  $("#restartSteamVR").hidden = !sys.steamvr.running;
  $("#getSteamVR").hidden = sys.steamvr.installed;
}
$$("#runtimeSeg input").forEach((b) => b.addEventListener("change", async () => {
  toast("Accept the Windows prompt to switch");
  const r = await request({ type: "setOpenXrRuntime", runtime: b.value });
  if (r.ok) toast(`OpenXR games now run through ${b.nextElementSibling.textContent}`);
  else showError("Nothing changed: Windows' permission prompt was declined or failed.");
  request({ type: "getState" });
}));
$("#registerDriver").onclick = registerDriver;
$("#getSteamVR").onclick = () => send({ type: "openUrl", url: "https://store.steampowered.com/app/250820/SteamVR/" });
$("#launchSteamVR").onclick = () => { send({ type: "launchSteamVR" }); toast("Starting SteamVR"); };
$("#restartSteamVR").onclick = async () => {
  if (await confirmAction("Restart SteamVR?", "Any running VR game closes.", "Restart")) { send({ type: "restartSteamVR" }); toast("Restarting SteamVR"); }
};
$("#openLogs").onclick = () => send({ type: "openLogs" });

function renderDevices() {
  const list = $("#devices");
  const devices = state.settings.trustedDevices || [];
  const key = JSON.stringify(devices);
  if (list.dataset.key === key) return;
  list.dataset.key = key;
  if (!devices.length) {
    list.innerHTML = `<li class="empty">None yet. Choose Always allow when a headset connects.</li>`;
    return;
  }
  list.innerHTML = devices.map((d) => `
    <li><div class="meta"><b>${esc(d.name)}</b><small>Last connected ${d.lastSeen ? new Date(d.lastSeen * 1000).toLocaleDateString() : "never"}</small></div>
    <button class="btn" data-remove="${esc(d.id)}" data-name="${esc(d.name)}" aria-label="Remove ${esc(d.name)}">Remove</button></li>`).join("");
  $$("[data-remove]", list).forEach((b) => (b.onclick = async () => {
    if (!(await confirmAction(`Remove ${b.dataset.name}?`, "It has to ask again the next time it connects.", "Remove"))) return;
    const r = await request({ type: "removeTrusted", deviceId: b.dataset.remove });
    if (r.ok === false) showError("Couldn't remove this headset. Try again.");
    else toast("Headset removed");
    request({ type: "getState" });
  }));
}

let authMode = "signIn";
let accountKey = "";
function renderAccount() {
  const view = $("#accountView");
  const c = state.cloud;
  const key = c.signedIn ? `in:${c.userName}:${c.email}` : `out:${authMode}`;
  if (key !== accountKey) {
    accountKey = key;  // otherwise leave the form alone: state updates must not erase typing
    if (c.signedIn) renderSignedIn(view, c);
    else renderSignedOut(view);
  }
  if (c.signedIn) {
    const allow = $("#allowRemote");
    allow.checked = state.settings.allowRemote;
    $("#remoteStatus").textContent = !state.settings.allowRemote
      ? "Off. Only your own network can reach this PC."
      : { online: "Online", connecting: "Connecting to the RinCast server…", error: c.error || "Can't reach the RinCast server." }[c.status] || "";
  }
}
function renderSignedIn(view, c) {
  view.innerHTML = `
    <div class="cols">
      <div>
        <h2 class="first">${esc(c.userName)}</h2>
        <p class="note selectable">${esc(c.email)}</p>
        <label class="check spaced"><input type="checkbox" data-setting="allowRemote" id="allowRemote"><span>Allow connections from other networks</span><small id="remoteStatus"></small></label>
        <div class="btns spaced"><button class="btn" id="signOut">Sign out</button></div>
      </div>
      <div>
        <h2 class="first">Sign in a headset</h2>
        <h3 id="codeLabel">Enter the code your headset shows</h3>
        <form class="code-entry" id="codeForm"><input type="text" id="code" maxlength="9" placeholder="XXXX-XXXX" autocomplete="off" spellcheck="false" aria-labelledby="codeLabel"><button class="btn primary" id="codeSubmit">Approve</button></form>
        <p class="failed" id="codeError" role="alert"></p>
      </div>
    </div>`;
  wireSetting($("#allowRemote"));
  $("#codeForm").onsubmit = async (e) => {
    e.preventDefault();
    $("#codeError").textContent = "";
    const submit = $("#codeSubmit");
    submit.disabled = true;
    submit.textContent = "Approving…";
    const r = await request({ type: "approveCode", code: $("#code").value });
    submit.disabled = false;
    submit.textContent = "Approve";
    if (r.ok) { toast("Headset signed in"); $("#code").value = ""; }
    else $("#codeError").textContent = r.error || "That code didn't work. Check it and try again.";
  };
  $("#signOut").onclick = async () => {
    if (!(await confirmAction("Sign out of this PC?", "Headsets can't reach it from other networks until you sign in again.", "Sign out"))) return;
    const r = await request({ type: "signOut" });
    if (r.ok === false) showError(r.error || "Couldn't sign out. Try again.");
    else toast("Signed out");
    request({ type: "getState" });
  };
}
function renderSignedOut(view) {
  const register = authMode === "register";
  view.innerHTML = `
    <h2 class="first">${register ? "Create an account" : "Sign in"}</h2>
    <p class="note auth-note">Only needed to reach this PC from other networks.</p>
    <form class="auth" id="authForm" novalidate>
      ${register ? '<label for="authName">Name</label><input type="text" id="authName" name="name" autocomplete="nickname">' : ""}
      <label for="authEmail">Email</label>
      <input type="email" id="authEmail" name="email" autocomplete="email" required aria-describedby="authError">
      <label for="authPassword">Password${register ? " (at least 8 characters)" : ""}</label>
      <input type="password" id="authPassword" name="password" autocomplete="${register ? "new-password" : "current-password"}" required aria-describedby="authError">
      <p class="error" id="authError" role="alert"></p>
      <button class="btn primary" id="authSubmit">${register ? "Create account" : "Sign in"}</button>
      <p class="switch-mode">${register ? 'Already have an account? <button type="button" class="link" id="toggleAuth">Sign in</button>' : 'New to RinCast? <button type="button" class="link" id="toggleAuth">Create an account</button>'}</p>
      ${register ? "" : '<p class="switch-mode">Forgot your password? Ask whoever runs your account server to reset it.</p>'}
    </form>`;
  $("#toggleAuth").onclick = () => { authMode = register ? "signIn" : "register"; renderAccount(); ($("#authName") || $("#authEmail")).focus(); };
  $("#authForm").onsubmit = async (e) => {
    e.preventDefault();
    const data = Object.fromEntries(new FormData(e.target));
    const err = $("#authError");
    err.textContent = "";
    if (!/^\S+@\S+\.\S+$/.test(data.email || "")) { err.textContent = "Enter a valid email address."; $("#authEmail").focus(); return; }
    if (register && (data.password || "").length < 8) { err.textContent = "Use at least 8 characters for the password."; $("#authPassword").focus(); return; }
    const btn = $("#authSubmit");
    const label = btn.textContent;
    btn.disabled = true;
    btn.textContent = register ? "Creating…" : "Signing in…";
    const r = await request({ type: authMode, ...data });
    btn.disabled = false;
    btn.textContent = label;
    if (!r.ok) { err.textContent = r.error || "That didn't work. Try again."; return; }
    toast(register ? "Account created" : "Signed in");
    request({ type: "getState" });
  };
}

$("#downloadLink").onclick = () => send({ type: "openUrl", url: state.downloadUrl });

function renderRail() {
  $("#downloadLink").textContent = `Get it from ${state.downloadSite}`;
  setFacts($("#aboutFacts"), [["Graphics card", state.system.gpu], state.system.encoder.backend && ["Video encoder", encoderText()], ["Version", state.version]]);
  $("#aboutLine").textContent = `Free and open source, MIT licensed. UDP port ${state.system.port}`;
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
request({ type: "getState" });

// ---------------------------------------------------------------- design-preview mock
function mock(msg) {
  const demo = new URLSearchParams(location.search).get("demo") || "connected";
  const none = demo === "firstrun" || demo === "problem" || demo === "signedout";
  if (demo === "firstrun") localStorage.removeItem("connectedBefore");
  const audio = { speaker: false, microphone: false, microphoneFeed: false, steam: { speaker: !(demo === "problem"), microphone: !(demo === "problem") }, active: demo === "problem" ? {} : { speaker: "steam", microphone: "steam" } };
  const base = mock.base ||= {
    type: "state", version: "1.0.0", downloadUrl: "https://rincast.com/#download", downloadSite: "rincast.com",
    settings: { pcName: "Studio PC", codec: "auto", tenBit: false, vrBitrateMbps: 200, desktopBitrateMbps: 40, adaptiveBitrate: true, fecPercent: 5, foveatedEncoding: true, adaptiveQuantization: true, twoPassEncoding: false, desktopMaxHeight: 0, streamAudio: true, playOnPcToo: false, micPassthrough: true, switchDefaultAudio: true, audioDevices: "auto", autoRegisterSteamVrDriver: true, requireApproval: true, startWithWindows: true, preventSleep: true, cloudUrl: "https://rincast.com", allowRemote: true,
      trustedDevices: none ? [] : [{ id: "q3", name: "Quest 3", model: "Meta Quest 3", lastSeen: Date.now() / 1000 }] },
    cloud: demo === "signedout" || demo === "firstrun" ? { signedIn: false } : { signedIn: true, userName: "Sam", email: "sam@example.com", status: demo === "problem" ? "error" : "online", error: demo === "problem" ? "The server didn't answer." : "", allowRemote: true },
    system: { gpu: demo === "problem" ? "Microsoft Basic Display Adapter" : "NVIDIA GeForce RTX 5070 Ti", port: 38711, addresses: ["192.168.1.20"], vpnAddresses: demo === "vpn" ? [{ name: "Tailscale", address: "100.101.102.103" }] : [], firewall: { otherNetworksOpen: demo !== "problem", publicInUse: demo === "public", publicAllowed: false, publicUnguarded: false }, encoder: demo === "problem" ? {} : { backend: "nvenc", h264: true, hevc: true, av1: true, hevc10: true },
      steamvr: { installed: true, running: false, driverRegistered: demo !== "problem" }, openxr: { rincastAvailable: false, rincastActive: false, steamvrActive: true },
      audioDriver: audio },
    session: demo === "connected" || demo === "lowbandwidth" ? { deviceName: "Quest 3", model: "Meta Quest 3", address: "192.168.1.42", remote: demo === "lowbandwidth", connectedSeconds: 1460, rttMs: demo === "lowbandwidth" ? 42.6 : 3.4, vrState: "running", vrApp: "Half-Life: Alyx",
      headsetBitrateMbps: 0, headsetCodec: "auto", vr: { fps: demo === "lowbandwidth" ? 45 : 90, encodeMs: 3.1, mbps: demo === "lowbandwidth" ? 7 : 182, codec: "HEVC", eyeWidth: 1536, eyeHeight: 1632, refreshRate: 90, latencyMs: demo === "lowbandwidth" ? 96 : 38 }, desktop: [],
      client: { latencyStages: demo === "lowbandwidth" ? { uplink: 21.3, game: 12.1, encoderWait: 0.7, encode: 4.4, downlink: 41.8, decode: 4.6, nextFrame: 3.1, compositor: 8 } : { uplink: 1.7, game: 11.4, encoderWait: 0.6, encode: 4.1, downlink: 5.3, decode: 4.2, nextFrame: 2.9, compositor: 7.8 } } } : null,
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
