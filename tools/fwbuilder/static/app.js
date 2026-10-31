// SPDX-FileCopyrightText: Copyright (c) 2026 Petr Vavrin
// SPDX-License-Identifier: MIT

const $ = (sel) => document.querySelector(sel);

const state = {
  boards: [],
  board: null,
  info: null,          // {target, uf2, modules: [{name, value, group, info: {cs, en}, source}]}
  selection: {},       // name -> bool, what will be built
  profiles: {},
  firmwares: [],
  ports: [],
  stream: null,
  job: null,
};

const GROUPS = ["fork", "module", "other"];

async function api(path, options = {}) {
  const res = await fetch(path, {
    headers: { "Content-Type": "application/json" },
    ...options,
    body: options.body ? JSON.stringify(options.body) : undefined,
  });
  const data = await res.json().catch(() => null);
  if (!res.ok) throw new Error((data && data.error) || res.statusText);
  return data;
}

function remember(key, value) {
  try { localStorage.setItem("fwb." + key, value); } catch (e) { /* storage blocked: only a convenience */ }
}
function recall(key) {
  try { return localStorage.getItem("fwb." + key); } catch (e) { return null; }
}

function el(tag, attrs = {}, ...children) {
  const node = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs)) {
    if (k === "class") node.className = v;
    else if (k.startsWith("on")) node.addEventListener(k.slice(2), v);
    else if (v !== false && v != null) node.setAttribute(k, v === true ? "" : v);
  }
  for (const c of children.flat()) {
    if (c === "" || c == null) continue;
    node.append(c instanceof Node ? c : document.createTextNode(c));
  }
  return node;
}

function fmtBytes(n) {
  if (n == null) return "?";
  return Math.abs(n) >= 1024 * 1024 ? (n / 1048576).toFixed(2) + " MB" : (n / 1024).toFixed(1) + " kB";
}

const short = (name) => name.replace(/^CIRCUITPY_/, "");

// --- language -------------------------------------------------------------------

function setLang(lang) {
  LANG = lang;
  remember("lang", lang);
  $("#lang").value = lang;
  applyStatic();
  renderBoardOptions();
  renderProfiles();
  renderModules();
  if (state.firmwares.length || $("#view-firmware").classList.contains("active")) renderFirmwares();
  setStatus(state.job);
  renderPorts();
}

// --- views -------------------------------------------------------------------

function showView(name) {
  document.querySelectorAll(".rail-button").forEach((b) => b.classList.toggle("active", b.dataset.view === name));
  document.querySelectorAll(".view").forEach((v) => v.classList.toggle("active", v.id === "view-" + name));
  $("#drawer").classList.toggle("active", name === "build");
  $("#workspace").classList.toggle("panel-open", name === "build");
  if (name === "firmware") loadFirmwares();
  remember("view", name);
}

// --- tooltip ---------------------------------------------------------------------

const tooltip = $("#tooltip");
let tooltipFor = null;

function showTooltip(anchor, content) {
  tooltipFor = anchor;
  tooltip.replaceChildren(...content);
  tooltip.hidden = false;
  const r = anchor.getBoundingClientRect();
  const w = tooltip.offsetWidth;
  const h = tooltip.offsetHeight;
  let left = Math.min(window.innerWidth - w - 8, Math.max(8, r.left + r.width / 2 - w / 2));
  let top = r.bottom + 8;
  if (top + h > window.innerHeight - 8) top = r.top - h - 8;
  tooltip.style.left = left + "px";
  tooltip.style.top = top + "px";
}

function hideTooltip() {
  tooltip.hidden = true;
  tooltipFor = null;
}

function chipName(target) {
  return target.replace(/^esp32/, "ESP32-").replace(/-$/, "").toUpperCase();
}

// Why a switch cannot be on for this board: what fork.mk and the chip rule out,
// then the switches it needs that are off now.
function blockedReasons(m) {
  const chip = chipName(state.info.target);
  const reasons = (m.blocked || []).map((b) => {
    if (b.kind === "target") return t("blocked.target", b.value.map(chipName).join(", "));
    if (b.kind === "soc") return t("blocked.soc", chip, b.value);
    if (b.kind === "file") return t("blocked.file", chip, b.value.replace("$(IDF_TARGET)", state.info.target));
    return t("blocked.needs", short(b.value));
  });
  for (const n of m.needs || []) if (!state.selection[n]) reasons.push(t("blocked.needs", short(n)));
  return reasons;
}

// Turns off whatever the last change left without what it needs, in a chain
// (IEEE802154 off takes ZIGBEE with it).
function dropBlocked() {
  let again = true;
  while (again) {
    again = false;
    for (const m of state.info.modules) {
      if (state.selection[m.name] && blockedReasons(m).length) {
        state.selection[m.name] = false;
        again = true;
      }
    }
  }
}

function moduleTooltip(m) {
  const desc = (m.info && (m.info[LANG] || m.info.en)) || t("modules.noInfo");
  const reasons = blockedReasons(m);
  return [
    el("div", { class: "tt-name" }, m.name),
    el("div", {}, desc),
    reasons.length ? el("div", { class: "tt-blocked" }, reasons.join(" · ")) : "",
    m.source ? el("div", { class: "tt-source" }, el("span", {}, t("modules.source") + ": "), m.source) : "",
  ];
}

function infoButton(m) {
  const b = el("button", { type: "button", class: "info", "aria-label": "info" }, "i");
  b.addEventListener("mouseenter", () => showTooltip(b, moduleTooltip(m)));
  b.addEventListener("mouseleave", hideTooltip);
  b.addEventListener("focus", () => showTooltip(b, moduleTooltip(m)));
  b.addEventListener("blur", hideTooltip);
  b.addEventListener("click", (e) => {
    e.preventDefault();
    if (tooltipFor === b) hideTooltip(); else showTooltip(b, moduleTooltip(m));
  });
  return b;
}

// --- boards and modules ------------------------------------------------------

async function loadBoards() {
  state.boards = await api("/api/boards");
  // One group per port: the whole port first, then its chips.
  const ports = [...new Set(state.boards.map((b) => b.port))];
  const chipSel = $("#chip-filter");
  chipSel.replaceChildren(el("option", { value: "" }, t("board.allChips")),
    ...ports.map((p) => el("optgroup", { label: p },
      el("option", { value: "port:" + p }, t("board.allOfPort", p)),
      ...[...new Set(state.boards.filter((b) => b.port === p).map((b) => b.chip))].sort()
        .map((c) => el("option", { value: c }, c)))));
  chipSel.value = recall("chip") || "";
  const last = recall("board");
  state.board = last && state.boards.some((b) => b.id === last) ? last : null;
  renderBoardOptions();
  await selectBoard($("#board").value);
}

function filteredBoards() {
  const q = $("#board-search").value.trim().toLowerCase();
  const chip = $("#chip-filter").value;
  const matches = (b) => !chip || (chip.startsWith("port:") ? b.port === chip.slice(5) : b.chip === chip);
  return state.boards.filter((b) => matches(b) &&
    (!q || b.name.toLowerCase().includes(q) || b.id.includes(q)));
}

function renderBoardOptions() {
  if (!state.boards.length) return;
  const sel = $("#board");
  const boards = filteredBoards();
  // The selected board stays listed, so a filter never changes it by itself.
  const current = state.boards.find((b) => b.id === state.board);
  if (current && !boards.includes(current)) boards.unshift(current);
  sel.replaceChildren(...boards.map((b) => el("option", { value: b.id }, `${b.name}  [${b.chip}]`)));
  if (!boards.length) sel.append(el("option", { value: "", disabled: true }, t("board.none")));
  if (state.board) sel.value = state.board;
  $("#chip-filter").options[0].textContent = t("board.allChips");
  for (const o of $("#chip-filter").options) {
    if (o.value.startsWith("port:")) o.textContent = t("board.allOfPort", o.value.slice(5));
  }
  const shown = filteredBoards().length;
  $("#board-search").title = t("board.count", shown);
  if (state.info) {
    const b = state.boards.find((x) => x.id === state.board);
    $("#board-info").textContent = `${state.info.port} · ${b ? b.target : state.info.target} · ` +
      (state.info.uf2 ? t("board.uf2") : t("board.noUf2")) + ` · ${t("board.count", shown)}`;
  }
}

async function selectBoard(id, overrides = {}) {
  if (!id) return;
  state.board = id;
  remember("board", id);
  $("#modules").replaceChildren(el("p", { class: "empty" }, t("modules.loading")));
  state.info = await api(`/api/boards/${id}/modules`);
  state.selection = {};
  for (const m of state.info.modules) state.selection[m.name] = m.value;
  for (const [name, value] of Object.entries(overrides)) {
    if (name in state.selection) state.selection[name] = !!value;
  }
  dropBlocked();
  renderBoardOptions();
  renderModules();
}

function overrides() {
  const out = {};
  if (!state.info) return out;
  for (const m of state.info.modules) {
    if (state.selection[m.name] !== m.value) out[m.name] = state.selection[m.name];
  }
  return out;
}

function renderModules() {
  const container = $("#modules");
  if (!state.info) {
    container.replaceChildren(el("p", { class: "empty" }, t("modules.choose")));
    return;
  }
  const filter = $("#filter").value.trim().toLowerCase();
  const onlyChanged = $("#only-changed").checked;
  const showOther = $("#show-other").checked;
  const showBlocked = $("#show-blocked").checked;
  const changed = overrides();
  container.replaceChildren();
  let shown = 0;
  let enabled = 0;
  const blocked = new Map(state.info.modules.map((m) => [m.name, blockedReasons(m)]));
  const blockedCount = [...blocked.values()].filter((r) => r.length).length;
  $("#blocked-count").textContent = blockedCount ? ` (${blockedCount})` : "";
  for (const m of state.info.modules) if (state.selection[m.name]) enabled++;
  const text = (m) => [m.name, (m.info && m.info[LANG]) || "", m.source || ""].join(" ").toLowerCase();

  for (const group of GROUPS) {
    if (group === "other" && !showOther && !filter) continue;
    const items = state.info.modules.filter((m) => m.group === group &&
      (showBlocked || !blocked.get(m.name).length) &&
      (!filter || text(m).includes(filter)) && (!onlyChanged || m.name in changed));
    if (!items.length) continue;
    shown += items.length;
    const grid = el("div", { class: "modules" });
    for (const m of items) {
      const reasons = blocked.get(m.name);
      const input = el("input", { type: "checkbox", onchange: (e) => {
        state.selection[m.name] = e.target.checked;
        dropBlocked();
        renderModules();
      } });
      input.checked = state.selection[m.name];
      input.disabled = reasons.length > 0;
      const isChanged = m.name in changed;
      const desc = m.group === "fork" && m.info ? (m.info[LANG] || m.info.en) : "";
      grid.append(el("div", { class: "module" + (isChanged ? " changed" : "") + (reasons.length ? " blocked" : "") },
        el("label", { class: "switch" }, input, el("span")),
        el("div", { class: "text" },
          el("div", { class: "name" }, short(m.name)),
          reasons.length ? el("div", { class: "why" }, reasons[0]) : "",
          desc ? el("div", { class: "desc" }, desc) : "",
          isChanged ? el("div", { class: "badge" }, t("modules.default", t(m.value ? "modules.on" : "modules.off"))) : ""),
        infoButton(m)));
    }
    container.append(el("section", { class: "group" }, el("h4", {}, `${t("group." + group)} (${items.length})`), grid));
  }
  if (!shown) container.append(el("p", { class: "empty" }, t("modules.noMatch")));
  $("#module-count").textContent = t("modules.count", enabled, state.info.modules.length);
  renderChanges(changed);
}

function renderChanges(changed) {
  const names = Object.keys(changed).sort();
  $("#changes-summary").textContent = names.length ? t("changes.some", names.length) : t("changes.none");
  $("#changes-list").replaceChildren(...names.map((n) =>
    el("li", { class: changed[n] ? "on" : "off" }, `${changed[n] ? "+" : "−"} ${short(n)}`)));
}

// --- profiles -----------------------------------------------------------------

async function loadProfiles(select) {
  state.profiles = await api("/api/profiles");
  renderProfiles(select);
}

function renderProfiles(select) {
  const sel = $("#profile");
  const current = select !== undefined ? select : sel.value;
  sel.replaceChildren(el("option", { value: "" }, t("profile.none")),
    ...Object.keys(state.profiles).sort().map((n) => el("option", { value: n }, `${n}  (${state.profiles[n].board})`)));
  sel.value = current || "";
}

async function applyProfile(name) {
  remember("profile", name);
  if (!name) return selectBoard(state.board);
  const p = state.profiles[name];
  $("#profile-name").value = name;
  await selectBoard(p.board, p.overrides);
}

async function saveProfile() {
  const name = $("#profile-name").value.trim() || $("#profile").value;
  if (!name) return alert(t("profile.needName"));
  await api("/api/profiles", { method: "POST", body: { name, board: state.board, overrides: overrides() } });
  await loadProfiles(name);
  remember("profile", name);
}

async function deleteProfile() {
  const name = $("#profile").value;
  if (!name || !confirm(t("profile.confirmDelete", name))) return;
  await api(`/api/profiles/${encodeURIComponent(name)}`, { method: "DELETE" });
  await loadProfiles("");
}

// --- jobs ---------------------------------------------------------------------

function setStatus(job) {
  state.job = job;
  const s = $("#job-status");
  const busy = job && job.state === "running";
  s.className = "status " + (job ? job.state : "");
  s.textContent = job
    ? `${t(job.kind === "build" ? "status.build" : "status.flash")} ${job.title}: ${t("state." + job.state)}`
    : t("status.ready");
  $("#build").disabled = busy;
  $("#cancel").hidden = !busy;
  $("#log-title").textContent = job ? t("log.of", job.kind, job.title) : t("log.title");
}

function logLine(text) {
  const log = $("#log");
  const cls = /error|Error|failed|FAILED|overflow/.test(text) ? "err"
    : /bytes free|saved as|Hash of data verified|Hard resetting|bootloader drive|copied;/.test(text) ? "ok" : "";
  log.append(cls ? el("span", { class: cls }, text + "\n") : text + "\n");
  if ($("#log-follow").checked) log.scrollTop = log.scrollHeight;
}

function followJob(job) {
  if (state.stream) state.stream.close();
  $("#log").replaceChildren();
  setStatus(job);
  if (!job) return;
  const es = new EventSource("/api/job/stream");
  state.stream = es;
  es.onmessage = (e) => logLine(JSON.parse(e.data));
  es.addEventListener("end", (e) => {
    const end = JSON.parse(e.data);
    es.close();
    state.stream = null;
    setStatus(end);
    if (end.kind === "build" && end.state === "done") loadFirmwares();
  });
  es.onerror = () => { es.close(); state.stream = null; };
}

async function startBuild() {
  const job = await api("/api/build", {
    method: "POST",
    body: { board: state.board, overrides: overrides(), profile: $("#profile").value, clean: $("#clean-build").checked },
  }).catch((e) => alert(e.message));
  if (job) { followJob(job); showView("log"); }
}

async function cancelJob() {
  await api("/api/job/cancel", { method: "POST" });
}

// --- firmware -----------------------------------------------------------------

async function loadFirmwares() {
  state.firmwares = await api("/api/firmwares");
  renderFirmwares();
}

function renderFirmwares() {
  const list = $("#firmwares");
  if (!state.firmwares.length) {
    list.replaceChildren(el("p", { class: "empty" }, t("fw.none")));
    return;
  }
  list.replaceChildren(...state.firmwares.map(renderFirmware));
}

function renderFirmware(fw) {
  const used = fw.flash_total ? fw.flash_total - fw.flash_free : null;
  const pct = fw.flash_total ? Math.min(100, (used / fw.flash_total) * 100) : 0;
  const changes = Object.entries(fw.overrides || {}).sort();
  const board = state.boards.find((b) => b.id === fw.board);
  return el("article", { class: "fw" },
    el("div", { class: "fw-head" },
      el("span", { class: "title" }, board ? board.name : fw.board),
      fw.profile ? el("span", { class: "tag on" }, fw.profile) : "",
      el("span", { class: "meta" }, `${fw.created.replace("T", " ")} · ${fw.port || "espressif"} · ${fw.target} · git ${fw.git}`)),
    fw.flash_total ? el("div", { class: "meta hint" },
      t("fw.flashUse", fmtBytes(used), fmtBytes(fw.flash_total), fmtBytes(fw.flash_free))) : "",
    fw.flash_total ? el("div", { class: "bar" + (pct > 97 ? " full" : "") }, el("div", { style: `width:${pct}%` })) : "",
    el("div", { class: "fw-actions" },
      ...fw.files.map((f) => el("a", { href: `/firmware/${fw.id}/${f.name}`, download: true },
        el("button", {}, t("fw.download", f.name.endsWith(".uf2") ? "UF2" : "BIN", fmtBytes(f.size))))),
      el("button", { class: "primary", onclick: () => openFlash(fw) }, t("fw.flash")),
      el("button", { class: "danger", onclick: () => deleteFirmware(fw) }, t("fw.delete"))),
    el("details", {},
      el("summary", {}, t("fw.contents", fw.enabled.length, changes.length)),
      changes.length ? el("div", { class: "taglist" },
        ...changes.map(([n, v]) => el("span", { class: "tag " + (v ? "on" : "off") }, short(n)))) : "",
      el("div", { class: "taglist" }, ...fw.enabled.map((n) => el("span", { class: "tag" }, short(n))))));
}

async function deleteFirmware(fw) {
  if (!confirm(t("fw.confirmDelete", fw.id))) return;
  await api(`/api/firmwares/${fw.id}`, { method: "DELETE" });
  loadFirmwares();
}

let flashTarget = null;

// Firmware of the ports other than espressif goes in as a .uf2 copied to the
// bootloader drive, not through esptool.
const isUf2Flash = (fw) => (fw.port || "espressif") !== "espressif";

function openFlash(fw) {
  flashTarget = fw;
  const uf2 = isUf2Flash(fw);
  $("#flash-what").textContent = `${fw.id} (${fw.target})`;
  const sel = $("#flash-port");
  sel.replaceChildren(...state.ports.map((p) => el("option", { value: p.port }, `${p.port}  ${p.desc || ""}`)));
  sel.value = $("#port").value;
  $("#flash-erase").checked = false;
  $("#flash-erase-row").hidden = uf2;
  $("#flash-uf2").hidden = !uf2;
  $("#flash-hint").textContent = t(uf2 ? "flash.hintUf2" : "flash.hint");
  if (uf2) loadDrives();
  $("#flash-dialog").showModal();
}

async function loadDrives() {
  const sel = $("#flash-drive");
  // CircuitPython sends an nRF board to its serial-only DFU, which has no drive.
  const touch = flashTarget && flashTarget.port !== "nordic";
  sel.replaceChildren(touch ? el("option", { value: "" }, t("flash.driveTouch"))
    : el("option", { value: "", disabled: true }, t("flash.driveNone")));
  let drives;
  try {
    drives = await api("/api/uf2drives");
  } catch (e) {
    sel.append(el("option", { value: "", disabled: true }, t("flash.drivesFailed", e.message)));
    return;
  }
  sel.append(...drives.map((d) => el("option", { value: d.drive }, `${d.drive}  ${d.board}`)));
  // A board already in its bootloader is the likely target.
  if (drives.length === 1) sel.value = drives[0].drive;
}

async function doFlash() {
  const port = $("#flash-port").value;
  const drive = isUf2Flash(flashTarget || {}) ? $("#flash-drive").value : "";
  if (!flashTarget || (!port && !drive)) return;
  if (port) {
    remember("port", port);
    $("#port").value = port;
  }
  const job = await api("/api/flash", {
    method: "POST", body: { firmware: flashTarget.id, port, drive, erase: $("#flash-erase").checked },
  }).catch((e) => alert(e.message));
  if (job) { followJob(job); showView("log"); }
}

// --- ports and board info ---------------------------------------------------------

async function loadPorts() {
  $("#port").replaceChildren(el("option", {}, t("port.loading")));
  try {
    state.ports = await api("/api/ports");
  } catch (e) {
    state.ports = [];
    alert(t("port.failed", e.message));
  }
  renderPorts();
}

function renderPorts() {
  const sel = $("#port");
  const current = sel.value || recall("port");
  sel.replaceChildren(...state.ports.map((p) => el("option", { value: p.port }, `${p.port}  ${p.desc || ""}`)));
  if (current && state.ports.some((p) => p.port === current)) sel.value = current;
}

async function probeBoard() {
  const port = $("#port").value;
  if (!port) return;
  $("#probe-title").textContent = t("probe.title", port);
  $("#probe-body").replaceChildren(el("p", { class: "hint" }, t("probe.running")));
  $("#probe-raw").textContent = "";
  $("#probe-raw-box").open = false;
  $("#probe-dialog").showModal();
  let res;
  try {
    res = await api(`/api/probe?port=${encodeURIComponent(port)}`);
  } catch (e) {
    $("#probe-body").replaceChildren(el("p", { class: "err-text" }, e.message));
    return;
  }
  $("#probe-raw").textContent = res.raw;
  if (!res.ok) {
    $("#probe-body").replaceChildren(el("p", { class: "err-text" }, t("probe.failed")));
    $("#probe-raw-box").open = true;
    return;
  }
  const i = res.info;
  let psram = i.psram;
  if (!psram && i.psram_cap) psram = i.psram_cap === "0" ? t("probe.psramNone") : t("probe.psramCode", i.psram_cap, i.psram_vendor || "?");
  if (psram === "none in package") psram = t("probe.psramNone");
  const rows = [
    ["probe.chip", i.chip],
    ["probe.features", i.features],
    ["probe.crystal", i.crystal],
    ["probe.flash", i.flash_size],
    ["probe.embeddedFlash", i.embedded_flash],
    ["probe.flashId", i.flash_manufacturer ? `${i.flash_manufacturer} / ${i.flash_device}` : ""],
    ["probe.psram", psram],
    ["probe.sram", i.sram_kb ? t("probe.sramNote", i.sram_kb) : ""],
    ["probe.package", i.package],
    ["probe.baseMac", i.base_mac],
    ["probe.mac", i.mac],
  ].filter(([, v]) => v);
  $("#probe-body").replaceChildren(el("table", { class: "info-table" },
    ...rows.map(([k, v]) => el("tr", {}, el("th", {}, t(k)), el("td", {}, v)))));
}

// --- start ---------------------------------------------------------------------

document.querySelectorAll(".rail-button").forEach((b) => b.addEventListener("click", () => showView(b.dataset.view)));
$("#lang").addEventListener("change", (e) => setLang(e.target.value));
$("#board").addEventListener("change", (e) => { $("#profile").value = ""; selectBoard(e.target.value); });
$("#board-search").addEventListener("input", renderBoardOptions);
$("#chip-filter").addEventListener("change", (e) => { remember("chip", e.target.value); renderBoardOptions(); });
$("#profile").addEventListener("change", (e) => applyProfile(e.target.value));
$("#profile-save").addEventListener("click", () => saveProfile().catch((e) => alert(e.message)));
$("#profile-delete").addEventListener("click", () => deleteProfile().catch((e) => alert(e.message)));
$("#reset-changes").addEventListener("click", () => selectBoard(state.board));
$("#filter").addEventListener("input", renderModules);
$("#only-changed").addEventListener("change", renderModules);
$("#show-other").addEventListener("change", renderModules);
$("#show-blocked").addEventListener("change", renderModules);
$("#build").addEventListener("click", startBuild);
$("#cancel").addEventListener("click", cancelJob);
$("#fw-refresh").addEventListener("click", loadFirmwares);
$("#ports-refresh").addEventListener("click", loadPorts);
$("#port").addEventListener("change", (e) => remember("port", e.target.value));
$("#probe").addEventListener("click", probeBoard);
$("#flash-drives-refresh").addEventListener("click", loadDrives);
$("#flash-dialog").addEventListener("close", () => { if ($("#flash-dialog").returnValue === "ok") doFlash(); });
document.addEventListener("scroll", hideTooltip, true);

(async () => {
  $("#lang").value = LANG;
  applyStatic();
  setStatus(null);
  await loadBoards();
  await loadProfiles(recall("profile") || "");
  if ($("#profile").value) await applyProfile($("#profile").value);
  loadPorts();
  const job = await api("/api/job");
  if (job) followJob(job);
  showView(recall("view") || "build");
})().catch((e) => alert(t("error.load", e.message)));
