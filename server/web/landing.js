// The landing page works without this. It adds what depends on the server and the visitor:
// real download links, the signed-in link, and looking around the hero with the pointer.

if (localStorage.getItem("rc.refresh")) {
  const account = document.getElementById("navAccount");
  account.textContent = "Your devices";
  account.href = "/account";
}

fetch("/api/v1/info")
  .then((r) => (r.ok ? r.json() : {}))
  .then((info) => {
    const foot = document.getElementById("footLinks");
    const link = (text, href) => {
      const a = document.createElement("a");
      a.textContent = text;
      a.href = href;
      foot.append(a);
    };
    if (info.privacyUrl) link("Privacy", info.privacyUrl);
    if (info.imprintUrl) link("Legal notice", info.imprintUrl);
    if (info.downloadUrl) {
      document.getElementById("notYet").hidden = true;
      document.querySelectorAll("[data-download]").forEach((a) => (a.href = info.downloadUrl));
      const source = document.getElementById("navSource");
      if (/github\.com/.test(info.downloadUrl)) {
        source.href = info.downloadUrl.replace(/\/releases.*$/, "");
        source.hidden = false;
      }
    }
  })
  .catch(() => {});

// Moving the pointer turns your head a little, as it would in the headset.
const reduced = matchMedia("(prefers-reduced-motion: reduce)").matches;
if (!reduced && matchMedia("(pointer: fine)").matches) {
  const world = document.getElementById("world");
  addEventListener("pointermove", (e) => {
    world.style.setProperty("--look-x", ((e.clientX / innerWidth - 0.5) * 2).toFixed(3));
    world.style.setProperty("--look-y", ((e.clientY / innerHeight - 0.5) * 2).toFixed(3));
  }, { passive: true });
}
