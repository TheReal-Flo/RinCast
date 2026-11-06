// RinCast account site: sign in / register, link a headset with its code,
// manage linked PCs and headsets and the account. No framework, no build step.

const app = document.getElementById("app");
const nav = document.getElementById("nav");

const session = {
  get access() { return localStorage.getItem("rc.access"); },
  get refresh() { return localStorage.getItem("rc.refresh"); },
  set(tokens) {
    localStorage.setItem("rc.access", tokens.accessToken);
    localStorage.setItem("rc.refresh", tokens.refreshToken);
  },
  clear() {
    localStorage.removeItem("rc.access");
    localStorage.removeItem("rc.refresh");
  },
  get signedIn() { return !!this.refresh; },
};

async function api(path, { method = "GET", body, auth = true } = {}) {
  const doFetch = () => fetch(`/api/v1${path}`, {
    method,
    headers: {
      "Content-Type": "application/json",
      ...(auth && session.access ? { Authorization: `Bearer ${session.access}` } : {}),
    },
    body: body ? JSON.stringify(body) : undefined,
  });
  let res;
  try {
    res = await doFetch();
    if (res.status === 401 && auth && session.refresh) {
      const r = await fetch("/api/v1/auth/refresh", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ refreshToken: session.refresh }),
      });
      if (r.ok) {
        session.set(await r.json());
        res = await doFetch();
      } else {
        session.clear();
        go("/login");
        throw new Error("You've been signed out. Sign in again.");
      }
    }
  } catch (e) {
    if (e instanceof TypeError) throw new Error("Can't reach the server. Check your connection and try again.");
    throw e;
  }
  const data = await res.json().catch(() => ({}));
  if (!res.ok) throw new Error(data.message || `The server couldn't do that (error ${res.status}).`);
  return data;
}

const h = (html) => {
  const t = document.createElement("template");
  t.innerHTML = html.trim();
  return t.content;
};
const esc = (s) => String(s ?? "").replace(/[&<>"']/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));

function setPage(title, content) {
  document.title = title ? `${title} - RinCast` : "RinCast";
  app.replaceChildren(content);
}

function go(path) {
  history.pushState({}, "", path);
  render(true);
}

document.addEventListener("click", (e) => {
  const a = e.target.closest("a[data-link]");
  if (a && a.origin === location.origin) {
    e.preventDefault();
    go(a.pathname + a.search);
  }
});
window.addEventListener("popstate", () => render(false));

const confirmDialog = document.getElementById("confirm");
function confirmAction(title, text, action) {
  document.getElementById("confirmTitle").textContent = title;
  document.getElementById("confirmText").textContent = text;
  const yes = document.getElementById("confirmYes");
  yes.textContent = action;
  confirmDialog.returnValue = "";
  confirmDialog.showModal();
  return new Promise((resolve) => {
    yes.onclick = () => confirmDialog.close("yes");
    document.getElementById("confirmNo").onclick = () => confirmDialog.close("no");
    confirmDialog.addEventListener("close", () => resolve(confirmDialog.returnValue === "yes"), { once: true });
  });
}

function renderNav() {
  const here = location.pathname;
  const cur = (p) => (here === p ? 'aria-current="page"' : "");
  nav.replaceChildren(session.signedIn
    ? h(`<a href="/account" data-link ${cur("/account")}>Devices</a>
         <a href="/link" data-link ${cur("/link")}>Link a headset</a>
         <a href="/settings" data-link ${cur("/settings")}>Account</a>
         <button id="signout">Sign out</button>`)
    : h(`<a href="/login" data-link ${cur("/login")}>Sign in</a>
         <a href="/register" data-link ${cur("/register")}>Create account</a>`));
  const so = document.getElementById("signout");
  if (so) so.onclick = async () => {
    try { await api("/auth/logout", { method: "POST", body: { refreshToken: session.refresh }, auth: false }); } catch {}
    session.clear();
    go("/login");
  };
}

// Details that arrive after the page may have rendered.
function fillOperatorDetails() {
  document.querySelectorAll("[data-download]").forEach((el) => {
    if (info.downloadUrl) el.replaceChildren(h(`<a href="${esc(info.downloadUrl)}">the RinCast releases page</a>`));
  });
  document.querySelectorAll("[data-reset-help]").forEach((el) => {
    el.textContent = info.contact ? `Forgot your password? Ask the server's operator: ${info.contact}.` : "Forgot your password? Whoever runs this server can reset it.";
  });
}

// Links the operator set (privacy policy, legal notice), if this server has them.
let info = {};
async function loadInfo() {
  try {
    const r = await fetch("/api/v1/info");
    if (!r.ok) return;
    info = await r.json();
    document.documentElement.dataset.signups = info.allowSignups === false ? "closed" : "open";
    const foot = document.getElementById("foot");
    if (info.privacyUrl) foot.append(h(`<a href="${esc(info.privacyUrl)}">Privacy</a>`));
    if (info.imprintUrl) foot.append(h(`<a href="${esc(info.imprintUrl)}">Legal notice</a>`));
    if (info.downloadUrl) foot.append(h(`<a href="${esc(info.downloadUrl)}">Download RinCast</a>`));
    fillOperatorDetails();
  } catch {}
}

function formPage({ title, sub, fields, submit, alt, className = "narrow", onSubmit }) {
  setPage(title, h(`
    <form class="${className}" novalidate>
      <h1>${title}</h1>
      <p class="sub">${sub}</p>
      ${fields}
      <div class="error" role="alert" id="formError"></div>
      <button class="btn primary" type="submit">${submit}</button>
      ${alt ? `<div class="alt">${alt}</div>` : ""}
    </form>`));
  const form = app.querySelector("form");
  const err = form.querySelector(".error");
  const btn = form.querySelector("button[type=submit]");
  form.querySelector("input")?.focus();
  form.onsubmit = async (e) => {
    e.preventDefault();
    err.textContent = "";
    form.querySelectorAll("input").forEach((i) => i.removeAttribute("aria-invalid"));
    // Say what's wrong with the first invalid field, in words.
    for (const input of form.querySelectorAll("input")) {
      if (!input.checkValidity()) {
        input.setAttribute("aria-invalid", "true");
        input.setAttribute("aria-describedby", "formError");
        const label = form.querySelector(`label[for="${input.id}"]`)?.dataset.name || "This field";
        err.textContent = input.validity.valueMissing ? `${label} is required.`
          : input.validity.typeMismatch ? "Enter a valid email address."
          : input.validity.tooShort ? `${label} needs at least ${input.minLength} characters.`
          : `Check ${label.toLowerCase()}.`;
        input.focus();
        return;
      }
    }
    btn.disabled = true;
    try {
      await onSubmit(Object.fromEntries(new FormData(form)), form);
    } catch (ex) {
      err.textContent = ex.message;
    } finally {
      btn.disabled = false;
    }
  };
}

function loginPage() {
  const next = new URLSearchParams(location.search).get("next") || "/account";
  formPage({
    title: "Sign in",
    sub: "Reach your PCs from any network. On your own network, RinCast works without an account.",
    fields: `
      <label for="email" data-name="Email">Email</label><input id="email" name="email" type="email" autocomplete="email" required>
      <label for="password" data-name="Password">Password</label><input id="password" name="password" type="password" autocomplete="current-password" required>`,
    submit: "Sign in",
    alt: `New to RinCast? <a href="/register${location.search}" data-link>Create an account</a>.
          <br><span data-reset-help>Forgot your password? Whoever runs this server can reset it.</span>`,
    onSubmit: async (v) => {
      session.set(await api("/auth/login", { method: "POST", body: { ...v, deviceKind: "web" }, auth: false }));
      go(next);
    },
  });
}

function registerPage() {
  if (info.allowSignups === false) {
    setPage("Create an account", h(`
      <div class="narrow">
        <h1>Sign-ups are closed</h1>
        <p class="sub">This server isn't taking new accounts. On your own network, RinCast works without one. To reach your PCs from other networks, you can run your own server.</p>
        <a class="btn" href="/login" data-link>Sign in</a>
      </div>`));
    return;
  }
  const next = new URLSearchParams(location.search).get("next") || "/account";
  formPage({
    title: "Create an account",
    sub: "One account for your PCs and headsets. There's no email to confirm, so remember your password.",
    fields: `
      <label for="name" data-name="Name">Name</label><input id="name" name="name" autocomplete="nickname" maxlength="60">
      <label for="email" data-name="Email">Email</label><input id="email" name="email" type="email" autocomplete="email" required>
      <label for="password" data-name="Password">Password</label><input id="password" name="password" type="password" autocomplete="new-password" minlength="8" required aria-describedby="pwHint">
      <div class="field-hint" id="pwHint">At least 8 characters.</div>`,
    submit: "Create account",
    alt: `Already have an account? <a href="/login${location.search}" data-link>Sign in</a>.`,
    onSubmit: async (v) => {
      session.set(await api("/auth/register", { method: "POST", body: { ...v, deviceKind: "web" }, auth: false }));
      go(next);
    },
  });
}

function linkPage() {
  const params = new URLSearchParams(location.search);
  if (!session.signedIn) {
    go(`/login?next=${encodeURIComponent("/link" + location.search)}`);
    return;
  }
  formPage({
    title: "Link a headset",
    sub: "Open RinCast on your headset and choose Sign in. Then enter the code it shows.",
    fields: `<label for="code" data-name="The code">Code from your headset</label>
             <input class="code" id="code" name="code" maxlength="9" autocomplete="off" spellcheck="false" placeholder="XXXX-XXXX" required value="${esc(params.get("code") || "")}">`,
    submit: "Continue",
    onSubmit: async (v) => {
      const found = await api(`/device/lookup?code=${encodeURIComponent(v.code)}`);
      const name = found.deviceName || "this headset";
      if (!(await confirmAction(`Link ${name}?`, "It can then connect to the PCs on your account. Only link a headset that's yours.", "Link headset"))) return;
      await api("/device/approve", { method: "POST", body: { userCode: v.code } });
      setPage("Headset linked", h(`
        <div class="narrow">
          <div class="notice good" role="status">${esc(found.deviceName || "Your headset")} is linked.</div>
          <h1>Put it on</h1>
          <p class="sub">Your PCs appear on its home screen in a moment.</p>
          <a class="btn" href="/account" data-link>View devices</a>
        </div>`));
      app.focus();
    },
  });
}

function ago(ts) {
  const s = (Date.now() - new Date(ts).getTime()) / 1000;
  if (s < 90) return "just now";
  if (s < 3600) return `${Math.round(s / 60)} min ago`;
  if (s < 86400) return `${Math.round(s / 3600)} h ago`;
  return `${Math.round(s / 86400)} days ago`;
}

function errorCard(title, message, retry) {
  setPage(title, h(`
    <div class="narrow">
      <h1>${esc(title)}</h1>
      <div class="notice bad" role="alert">${esc(message)}</div>
      <button class="btn" id="retry">Try again</button>
    </div>`));
  document.getElementById("retry").onclick = retry;
}

async function accountPage() {
  if (!session.signedIn) return go("/login");
  setPage("Devices", h(`<div><h1>Devices</h1><p class="sub" role="status">Loading…</p></div>`));
  let me, pcs, headsets;
  try {
    [me, { pcs }, { headsets }] = await Promise.all([api("/me"), api("/pcs"), api("/headsets")]);
  } catch (e) {
    if (session.signedIn) errorCard("Devices", e.message, accountPage);
    return;
  }
  setPage("Devices", h(`
    <div>
      <div class="head"><div><h1>Devices</h1><p class="sub">Signed in as ${esc(me.name || me.email)}.</p></div>
      <a class="btn primary small" href="/link" data-link>Link a headset</a></div>
      <div class="error" role="alert" id="pageError"></div>
      <h2>PCs</h2>
      <ul class="list" id="pcs"></ul>
      <h2>Headsets</h2>
      <ul class="list" id="headsets"></ul>
    </div>`));
  const pcState = (pc) => (pc.busy ? '<span class="state busy">In use</span>' : pc.online ? '<span class="state on">Online</span>' : '<span class="state off">Offline</span>');
  document.getElementById("pcs").replaceChildren(pcs.length ? h(pcs.map((pc) => `
      <li class="row">
        <div class="meta"><div class="title">${esc(pc.name)} ${pcState(pc)}</div>
        <div class="detail">${pc.gpu ? `${esc(pc.gpu)}. ` : ""}${pc.online ? "" : `Last seen ${ago(pc.lastSeenAt)}.`}</div></div>
        <button class="btn small" data-pc="${esc(pc.id)}" data-name="${esc(pc.name)}" aria-label="Remove ${esc(pc.name)}">Remove</button>
      </li>`).join(""))
    : h(`<li class="empty">No PCs yet. Install RinCast Streamer on your PC and sign in there with this account.</li>`));
  document.getElementById("headsets").replaceChildren(headsets.length ? h(headsets.map((hs) => `
      <li class="row">
        <div class="meta"><div class="title">${esc(hs.name)}</div><div class="detail">${esc(hs.model || "Headset")}, linked ${ago(hs.createdAt)}</div></div>
        <button class="btn small" data-hs="${esc(hs.id)}" data-name="${esc(hs.name)}" aria-label="Remove ${esc(hs.name)}">Remove</button>
      </li>`).join(""))
    : h(`<li class="empty">No headsets yet. Open RinCast on your headset, choose Sign in, then <a href="/link" data-link>enter its code</a>.</li>`));

  const wire = (selector, title, text, path) => app.querySelectorAll(selector).forEach((b) => (b.onclick = async () => {
    if (!(await confirmAction(`Remove ${b.dataset.name}?`, text, "Remove"))) return;
    try {
      await api(path(b), { method: "DELETE" });
      accountPage();
    } catch (e) {
      document.getElementById("pageError").textContent = e.message;
    }
  }));
  wire("[data-pc]", "PC", "Headsets can't reach it over the internet until you sign in on it again.", (b) => `/pcs/${b.dataset.pc}`);
  wire("[data-hs]", "Headset", "It's signed out and has to link again to reach your PCs.", (b) => `/headsets/${b.dataset.hs}`);
}

async function settingsPage() {
  if (!session.signedIn) return go("/login");
  setPage("Account", h(`<div class="narrow"><h1>Account</h1><p class="sub" role="status">Loading…</p></div>`));
  let me;
  try {
    me = await api("/me");
  } catch (e) {
    if (session.signedIn) errorCard("Account", e.message, settingsPage);
    return;
  }
  setPage("Account", h(`
    <div class="narrow">
      <h1>Account</h1>
      <p class="sub">${esc(me.email)}</p>

      <h2>Change password</h2>
      <form id="passwordForm" novalidate>
        <label for="currentPassword">Current password</label>
        <input id="currentPassword" name="currentPassword" type="password" autocomplete="current-password" required>
        <label for="newPassword">New password</label>
        <input id="newPassword" name="newPassword" type="password" autocomplete="new-password" minlength="8" required aria-describedby="newPwHint">
        <div class="field-hint" id="newPwHint">At least 8 characters. Other devices are signed out.</div>
        <div class="error" role="alert" id="passwordError"></div>
        <div class="field-hint" role="status" id="passwordDone"></div>
        <button class="btn full" id="passwordBtn">Change password</button>
      </form>

      <h2>Your data</h2>
      <p class="sub">Download what this server stores about you, as a JSON file.</p>
      <button class="btn" id="exportData">Download my data</button>
      <div class="error" role="alert" id="exportError"></div>

      <h2>Delete account</h2>
      <p class="sub">Removes your account, PCs and headsets from this server. This can't be undone.</p>
      <form id="deleteForm" novalidate>
        <label for="deletePassword">Password</label>
        <input id="deletePassword" name="password" type="password" autocomplete="current-password" required>
        <div class="error" role="alert" id="deleteError"></div>
        <button class="btn danger full" id="deleteBtn">Delete my account</button>
      </form>
    </div>`));
  document.getElementById("passwordForm").onsubmit = async (e) => {
    e.preventDefault();
    const err = document.getElementById("passwordError");
    const done = document.getElementById("passwordDone");
    err.textContent = done.textContent = "";
    const v = Object.fromEntries(new FormData(e.target));
    if (!v.currentPassword) { err.textContent = "Enter your current password."; return; }
    if ((v.newPassword || "").length < 8) { err.textContent = "Use at least 8 characters for the new password."; return; }
    try {
      await api("/me", { method: "PATCH", body: v });
      e.target.reset();
      done.textContent = "Password changed. Your other devices have been signed out.";
    } catch (ex) {
      err.textContent = ex.message;
    }
  };
  document.getElementById("exportData").onclick = async () => {
    const err = document.getElementById("exportError");
    err.textContent = "";
    try {
      const data = await api("/me/export");
      const url = URL.createObjectURL(new Blob([JSON.stringify(data, null, 2)], { type: "application/json" }));
      const a = document.createElement("a");
      a.href = url;
      a.download = "rincast-account.json";
      a.click();
      URL.revokeObjectURL(url);
    } catch (e) {
      err.textContent = e.message;
    }
  };
  document.getElementById("deleteForm").onsubmit = async (e) => {
    e.preventDefault();
    const err = document.getElementById("deleteError");
    const password = document.getElementById("deletePassword").value;
    err.textContent = "";
    if (!password) { err.textContent = "Enter your password to delete the account."; return; }
    if (!(await confirmAction("Delete your account?", "Your PCs and headsets are signed out and everything is removed. This can't be undone.", "Delete account"))) return;
    try {
      await api("/me", { method: "DELETE", body: { password } });
      session.clear();
      go("/login");
    } catch (ex) {
      err.textContent = ex.message;
    }
  };
}

function render(moveFocus) {
  renderNav();
  queueMicrotask(fillOperatorDetails);
  const path = location.pathname;
  if (path === "/login") loginPage();
  else if (path === "/register") registerPage();
  else if (path === "/link") linkPage();
  else if (path === "/account") accountPage();
  else if (path === "/settings") settingsPage();
  else if (session.signedIn) accountPage();
  else loginPage();
  if (moveFocus && !app.contains(document.activeElement)) app.focus();
}

render(false);
loadInfo().then(() => {
  // Only these depend on what the server says; never re-render a form someone is typing in.
  if (info.allowSignups === false && location.pathname === "/register") render(false);
});
