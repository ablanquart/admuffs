// SPDX-License-Identifier: MIT
#include "web/web_server.h"
#include "common.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cstring>
#include <ctime>
#include <map>

namespace admuffs {

namespace {

// ---------------------------------------------------------------------------
// The web app. Self-contained: inline CSS/JS, no external assets. One page
// with stacked "panels": main remote, ADMUFFS SETTINGS hub, and its four
// sub-panels (audio sampling, remote-code recording, configuration, info).
// ---------------------------------------------------------------------------
const char* kRemotePage = R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>admuffs remote</title>
<style>
  :root { --bg:#111418; --panel:#1c2128; --btn:#2a313b; --btn-hi:#39424e;
          --accent:#4c8dff; --ok:#2ea043; --err:#d64545; --fg:#e6e9ee; }
  * { box-sizing:border-box; -webkit-tap-highlight-color:transparent; }
  body { margin:0; background:var(--bg); color:var(--fg);
         font-family:system-ui,-apple-system,sans-serif;
         display:flex; justify-content:center; min-height:100vh; }
  .remote { width:290px; padding:18px 16px 26px; background:var(--panel);
            margin:16px 0; border-radius:26px; box-shadow:0 8px 30px #0008; }
  h1 { font-size:15px; text-align:center; margin:2px 0 2px; font-weight:600;
       letter-spacing:.08em; color:#9aa4b2; }
  #status { font-size:11px; text-align:center; color:#6b7684; margin:0 0 14px;
            min-height:14px; overflow:hidden; text-overflow:ellipsis;
            white-space:nowrap; }
  button { border:0; border-radius:12px; background:var(--btn); color:var(--fg);
           font-size:17px; padding:12px 0; cursor:pointer; width:100%;
           transition:background .12s, transform .05s; user-select:none; }
  button:active { transform:scale(.96); background:var(--btn-hi); }
  button.flash-ok { background:var(--ok); }
  button.flash-err { background:var(--err); }
  .row { display:grid; gap:10px; margin-bottom:10px; }
  .r2 { grid-template-columns:1fr 1fr; }
  .r3 { grid-template-columns:1fr 1fr 1fr; }
  .power { background:#3a2327; color:#ff7b72; font-weight:700; }
  .input { font-size:13px; }
  .dpad { display:grid; grid-template-columns:1fr 1fr 1fr;
          grid-template-rows:repeat(3, 60px); gap:8px; margin:4px 0 12px;
          align-items:center; }
  .dpad button { height:52px; padding:0; font-size:19px; }
  .dpad .ok { background:var(--accent); color:#fff; font-weight:700;
              border-radius:50%; width:64px; height:64px; justify-self:center; }
  .dpad .blank { visibility:hidden; }
  .rockers { display:grid; grid-template-columns:1fr 1fr 1fr; gap:10px;
             margin-bottom:12px; align-items:stretch; }
  .rocker { display:flex; flex-direction:column; gap:6px; }
  .rocker .lbl { text-align:center; font-size:11px; color:#8b95a3;
                 letter-spacing:.1em; }
  .mutebtn { background:#243447; font-size:14px; }
  .digits { grid-template-columns:1fr 1fr 1fr; }
  .digits button { font-size:16px; padding:10px 0; }
  .auxrow { margin-top:2px; }
  .auxrow .aux { font-size:11px; color:#9aa4b2; padding:11px 0;
                 letter-spacing:.04em; }
  #automute { display:flex; align-items:center; justify-content:center;
              gap:8px; margin:0 auto 14px; padding:8px 14px; width:auto;
              max-width:220px; border-radius:999px; font-size:12px;
              letter-spacing:.06em; background:var(--btn); color:#8b95a3; }
  #automute .dot { width:9px; height:9px; border-radius:50%;
                   background:#555e6a; transition:background .15s; }
  #automute.on { color:#bfe6c8; background:#1d3324; }
  #automute.on .dot { background:var(--ok); }
  #automute.off { color:#f0b9b9; background:#3a2426; }
  #automute.off .dot { background:var(--err); }
  /* panels */
  .panel { display:none; }
  .panel-title { text-align:center; font-size:13px; font-weight:600;
                 letter-spacing:.1em; color:#9aa4b2; margin:6px 0 4px; }
  .panel-help { text-align:center; font-size:10px; color:#6b7684;
                margin-bottom:14px; }
  .menu-btn { display:block; width:100%; margin-bottom:10px; padding:12px 0;
              font-size:12px; letter-spacing:.08em; color:#9aa4b2;
              background:#20262e; border:1px dashed #39424e; line-height:1.5; }
  .menu-btn span { display:block; font-size:9px; letter-spacing:.03em;
                   color:#6b7684; margin-top:3px; }
  .back-btn { margin-top:14px; font-size:12px; color:#9aa4b2;
              background:var(--btn); }
  .launcher { margin-top:14px; font-size:11px; letter-spacing:.08em;
              color:#9aa4b2; background:#20262e; border:1px dashed #39424e;
              padding:9px 0; }
  #calinfo, #recinfo, #cfginfo { font-size:10px; text-align:center;
              color:#6b7684; margin-top:8px; min-height:12px; }
  /* config rows */
  .cfg-row { display:flex; align-items:center; gap:6px; margin-bottom:6px; }
  .cfg-row .k { flex:1; font-size:10px; color:#8b95a3; letter-spacing:.03em; }
  .cfg-row button { flex:1.2; font-size:10px; padding:7px 4px;
                    background:#20262e; border:1px solid #39424e;
                    overflow:hidden; text-overflow:ellipsis; white-space:nowrap; }
  .cfg-row .dirty { border-color:#c9a227; color:#e6cf7a; }
  #restart-btn { background:#3a2327; color:#ff7b72; font-size:12px;
                 margin-top:10px; }
  /* record */
  select { width:100%; padding:10px; border-radius:10px; background:#20262e;
           color:var(--fg); border:1px solid #39424e; font-size:13px;
           margin-bottom:10px; }
  #rec-remote { display:flex; flex-direction:column; gap:6px; margin-bottom:10px; }
  .rec-row { display:grid; gap:6px; }
  .rec-btn { position:relative; font-size:11px; padding:10px 0; border-radius:9px;
             background:#20262e; border:1px solid #39424e; color:#9aa4b2;
             letter-spacing:.03em; }
  .rec-btn.blank { visibility:hidden; }
  .rec-btn.sel { border-color:var(--accent); color:#fff; background:#233247; }
  .rec-btn .done { position:absolute; top:4px; right:5px; width:6px; height:6px;
                   border-radius:50%; background:var(--ok); }
  #rec-custom { display:flex; flex-wrap:wrap; gap:6px; margin-bottom:10px; }
  .rec-chip { position:relative; font-size:10px; padding:8px 12px; border-radius:999px;
              background:#20262e; border:1px dashed #4c8dff66; color:#9aa4b2;
              width:auto; }
  .rec-chip.sel { border-style:solid; border-color:var(--accent); color:#fff;
                  background:#233247; }
  .rec-chip .done { position:absolute; top:2px; right:4px; width:6px; height:6px;
                    border-radius:50%; background:var(--ok); }
  #rec-selected { font-size:11px; text-align:center; color:#9aa4b2;
                  margin-bottom:8px; }
  #rec-selected b { color:#e6e9ee; }
  /* info rows */
  .inf-row { display:flex; justify-content:space-between; font-size:11px;
             margin-bottom:6px; gap:10px; }
  .inf-row .k { color:#8b95a3; white-space:nowrap; }
  .inf-row .v { color:#cdd5df; text-align:right; overflow:hidden;
                text-overflow:ellipsis; }
  .inf-sec { font-size:10px; letter-spacing:.1em; color:#6b7684;
             margin:12px 0 6px; text-align:center; }
  #logbox { height:340px; overflow-y:auto; background:#0c0f13;
            border:1px solid #262d36; border-radius:10px; padding:8px;
            font-family:ui-monospace,Menlo,Consolas,monospace; font-size:9px;
            line-height:1.55; color:#9fb0c0; white-space:pre-wrap;
            word-break:break-all; }
  #logbox .warn { color:#e6cf7a; } #logbox .err { color:#ff8d8d; }
  #login { position:fixed; inset:0; background:#0b0e12; z-index:99;
           display:flex; align-items:center; justify-content:center; }
  .login-card { width:250px; background:var(--panel); padding:26px 22px;
                border-radius:20px; box-shadow:0 8px 30px #0008; text-align:center; }
  #pin { width:100%; padding:12px; margin:14px 0 10px; border-radius:12px;
         background:#0c0f13; border:1px solid #39424e; color:var(--fg);
         font-size:22px; text-align:center; letter-spacing:.5em; }
  #pin-go { background:var(--accent); color:#fff; font-weight:700; }
  #logout-btn { background:var(--btn); color:#9aa4b2; font-size:12px;
                margin-top:6px; }
</style>
</head>
<body>
<div id="login" style="display:none">
  <div class="login-card">
    <div class="panel-title">ADMUFFS</div>
    <div class="panel-help">Enter PIN to unlock the remote</div>
    <input id="pin" type="password" inputmode="numeric" autocomplete="off"
           maxlength="8" placeholder="PIN">
    <button id="pin-go">UNLOCK</button>
    <div id="loginmsg" class="panel-help" style="margin-top:10px">&nbsp;</div>
  </div>
</div>
<div class="remote">
  <h1>ADMUFFS</h1>
  <div id="status">connecting&hellip;</div>

  <button id="automute"><span class="dot"></span><span id="amlabel">AUTO-MUTE</span></button>

  <!-- ===================== main remote ===================== -->
  <div id="main" class="panel" style="display:block">
    <div class="row r2">
      <button class="power" data-k="Power">&#x23FB;</button>
      <button class="input" data-k="Input">INPUT</button>
    </div>

    <div class="dpad">
      <button class="blank"></button>
      <button data-k="Up">&#9650;</button>
      <button class="blank"></button>
      <button data-k="Left">&#9664;</button>
      <button class="ok" data-k="Ok">OK</button>
      <button data-k="Right">&#9654;</button>
      <button class="blank"></button>
      <button data-k="Down">&#9660;</button>
      <button class="blank"></button>
    </div>

    <div class="rockers">
      <div class="rocker">
        <div class="lbl">VOL</div>
        <button data-k="VolumeUp">+</button>
        <button data-k="VolumeDown">&minus;</button>
      </div>
      <div class="rocker">
        <div class="lbl">&nbsp;</div>
        <button class="mutebtn" data-k="Mute" style="flex:1">MUTE</button>
      </div>
      <div class="rocker">
        <div class="lbl">CH</div>
        <button data-k="ChannelUp">+</button>
        <button data-k="ChannelDown">&minus;</button>
      </div>
    </div>

    <div class="row digits">
      <button data-k="1">1</button><button data-k="2">2</button><button data-k="3">3</button>
      <button data-k="4">4</button><button data-k="5">5</button><button data-k="6">6</button>
      <button data-k="7">7</button><button data-k="8">8</button><button data-k="9">9</button>
      <button class="blank" disabled></button><button data-k="0">0</button><button class="blank" disabled></button>
    </div>

    <div class="row r3 auxrow">
      <button class="aux" data-k="Settings">&#9881; SETTINGS</button>
      <button class="aux" data-k="Home">&#8962; HOME</button>
      <button class="aux" data-k="Back">&#8617; BACK</button>
    </div>

    <button class="launcher" data-go="settings">&#9881; ADMUFFS SETTINGS &#9662;</button>
  </div>

  <!-- ===================== settings hub ===================== -->
  <div id="settings" class="panel">
    <div class="panel-title">ADMUFFS SETTINGS</div>
    <div class="panel-help">Everything the setup wizard can change, from your
      couch.</div>
    <button class="menu-btn" data-go="sampling">&#127908; AUDIO SAMPLING
      <span>calibrate detection &amp; volume target with the mic</span></button>
    <button class="menu-btn" data-go="record">&#128225; RECORD REMOTE CODES
      <span>teach admuffs your real remote via the IR receiver</span></button>
    <button class="menu-btn" data-go="config">&#128295; CONFIGURATION
      <span>TV, control method, audio, modes &mdash; the TUI settings</span></button>
    <button class="menu-btn" data-go="info">&#8505;&#65039; SYSTEM INFO
      <span>version, host, service state, pHAT sensor readings</span></button>
    <button class="menu-btn" data-go="log">&#128220; VIEW LOG
      <span>the live admuffs log, as you'd see it on the CLI</span></button>
    <button class="menu-btn" id="changepin-btn">&#128273; CHANGE PIN
      <span>set the web-remote unlock code</span></button>
    <button id="restart-btn">&#8635; RESTART ADMUFFS</button>
    <button id="logout-btn">&#128274; LOG OUT</button>
    <div id="svc-hint" class="panel-help" style="margin-top:8px"></div>
    <button class="back-btn" data-go="main">&#8617; BACK TO REMOTE</button>
  </div>

  <!-- ===================== audio sampling ===================== -->
  <div id="sampling" class="panel">
    <div class="panel-title">AUDIO SAMPLING</div>
    <div class="panel-help">Calibration tools &mdash; each press measures ~2 s of
      microphone audio.</div>
    <button class="menu-btn" id="s-level">FIX VOLUME TARGET
      <span>press at the volume you like (normalize mode)</span></button>
    <button class="menu-btn" id="s-normal">SAMPLE - NORMAL
      <span>press while regular programming is playing</span></button>
    <button class="menu-btn" id="s-commercial">SAMPLE - COMMERCIAL
      <span>press while an ad break is airing</span></button>
    <div id="calinfo">no calibration yet</div>
    <button class="back-btn" data-go="settings">&#8617; BACK TO SETTINGS</button>
  </div>

  <!-- ===================== record remote codes ===================== -->
  <div id="record" class="panel">
    <div class="panel-title">RECORD REMOTE CODES</div>
    <div class="panel-help">Tap the button you want to teach, then press RECORD
      and aim your TV's real remote at the pHAT's IR receiver. A green dot marks
      buttons already recorded; recorded keys override the built-in database
      immediately.</div>
    <div id="rec-remote"></div>
    <div id="rec-custom"></div>
    <button class="menu-btn" id="rec-add">&#43; ADD BUTTON
      <span>for a remote function not shown above</span></button>
    <div id="rec-selected">Selected: <b id="rec-sel-name">none</b></div>
    <button class="menu-btn" id="rec-go">&#9210; RECORD SELECTED
      <span>then press the button on your remote within 9 seconds</span></button>
    <div id="recinfo">&nbsp;</div>
    <button class="back-btn" data-go="settings">&#8617; BACK TO SETTINGS</button>
  </div>

  <!-- ===================== configuration ===================== -->
  <div id="config" class="panel">
    <div class="panel-title">CONFIGURATION</div>
    <div class="panel-help">Tap a value to change it. Yellow = saved, applies
      after RESTART ADMUFFS.</div>
    <div id="cfg-rows"></div>
    <div id="cfginfo">&nbsp;</div>
    <button class="back-btn" data-go="settings">&#8617; BACK TO SETTINGS</button>
  </div>

  <!-- ===================== live log ===================== -->
  <div id="log" class="panel">
    <div class="panel-title">LIVE LOG</div>
    <div id="logbox"></div>
    <button class="back-btn" data-go="settings">&#8617; BACK TO SETTINGS</button>
  </div>

  <!-- ===================== system info ===================== -->
  <div id="info" class="panel">
    <div class="panel-title">SYSTEM INFO</div>
    <div id="inf-rows" class="panel-help">loading&hellip;</div>
    <button class="menu-btn" id="inf-refresh">&#8635; REFRESH</button>
    <button class="back-btn" data-go="settings">&#8617; BACK TO SETTINGS</button>
  </div>
</div>

<script>
  const $ = id => document.getElementById(id);
  const status = $('status');

  // ---- panel switching ----
  const PANELS = ['main','settings','sampling','record','config','info','log'];
  function show(id) {
    PANELS.forEach(p => $(p).style.display = (p === id ? 'block' : 'none'));
    if (id === 'config') loadConfig();
    if (id === 'info') loadInfo();
    if (id === 'settings') loadSvcHint();
    if (id === 'record') loadRecorded();
    if (id === 'log') startLog(); else stopLog();
  }
  document.querySelectorAll('[data-go]').forEach(b =>
    b.addEventListener('click', () => show(b.dataset.go)));

  function flash(btn, ok) {
    const cls = ok ? 'flash-ok' : 'flash-err';
    btn.classList.add(cls);
    setTimeout(() => btn.classList.remove(cls), 350);
  }

  // ---- status + auto-mute pill ----
  const amBtn = $('automute'), amLabel = $('amlabel');
  function setAm(on) {
    amBtn.classList.toggle('on', on);
    amBtn.classList.toggle('off', !on);
    amLabel.textContent = 'AUTO-MUTE: ' + (on ? 'ON' : 'OFF');
  }
  function refreshStatus() {
    return fetch('/status').then(r => {
      if (r.status === 401) { $('login').style.display = 'flex'; return null; }
      $('login').style.display = 'none';
      return r.json();
    }).then(j => {
      if (!j) return;
      status.textContent = j.controller; setAm(j.automute !== false);
    }).catch(() => { status.textContent = 'status unavailable'; });
  }
  refreshStatus();

  function doLogin() {
    const pin = $('pin').value;
    $('loginmsg').textContent = 'checking…';
    fetch('/auth', { method:'POST',
      headers:{'Content-Type':'application/x-www-form-urlencoded'},
      body:'pin=' + encodeURIComponent(pin) })
      .then(r => r.json().then(j => ({s:r.status, j})))
      .then(({s,j}) => {
        if (j.ok) { $('pin').value=''; $('loginmsg').textContent=''; refreshStatus(); }
        else if (j.locked) $('loginmsg').textContent = 'too many tries — wait ' +
          j.retry_after + 's';
        else $('loginmsg').textContent = 'incorrect PIN';
      })
      .catch(() => { $('loginmsg').textContent = 'error'; });
  }
  $('pin-go').addEventListener('click', doLogin);
  $('pin').addEventListener('keydown', e => { if (e.key === 'Enter') doLogin(); });

  $('changepin-btn').addEventListener('click', () => {
    const cur = prompt('Current PIN'); if (cur === null) return;
    const nw = prompt('New PIN (4-8 digits)'); if (nw === null) return;
    const nw2 = prompt('Confirm new PIN'); if (nw2 === null) return;
    if (nw !== nw2) { alert('PINs do not match'); return; }
    fetch('/pin', { method:'POST',
      headers:{'Content-Type':'application/x-www-form-urlencoded'},
      body:'current=' + encodeURIComponent(cur) + '&new=' + encodeURIComponent(nw) })
      .then(r => r.json())
      .then(j => alert(j.ok ? 'PIN changed. You will need it next time you unlock.'
                            : 'Failed: ' + j.error))
      .catch(() => alert('request failed'));
  });

  $('logout-btn').addEventListener('click', () => {
    fetch('/logout', { method:'POST' }).finally(() => {
      $('login').style.display = 'flex'; show('main');
    });
  });
  amBtn.addEventListener('click', () =>
    fetch('/automute', { method: 'POST' }).then(r => r.json())
      .then(j => setAm(j.automute)).catch(() => {}));

  // ---- remote keys ----
  document.querySelectorAll('button[data-k]').forEach(btn =>
    btn.addEventListener('click', () =>
      fetch('/key/' + encodeURIComponent(btn.dataset.k), { method: 'POST' })
        .then(r => r.json()).then(j => flash(btn, j.ok))
        .catch(() => flash(btn, false))));

  // ---- audio sampling ----
  const calinfo = $('calinfo');
  [['s-normal', 'normal'], ['s-commercial', 'commercial']].forEach(([id, label]) => {
    $(id).addEventListener('click', () => {
      calinfo.textContent = 'sampling ' + label + '…';
      fetch('/sample/' + label, { method: 'POST' }).then(r => r.json())
        .then(j => {
          flash($(id), j.ok);
          if (!j.ok) { calinfo.textContent = j.error; return; }
          calinfo.textContent = label + ': ' + j.db.toFixed(1) + ' dB (avg ' +
            j.avg_db.toFixed(1) + ', n=' + j.n + ')' +
            (j.have_both ? ' → threshold ' + j.delta_db.toFixed(1) + ' dB'
                         : ' — now sample the other one');
        })
        .catch(() => { flash($(id), false); calinfo.textContent = 'request failed'; });
    });
  });
  $('s-level').addEventListener('click', () => {
    calinfo.textContent = 'sampling current level…';
    fetch('/sample/level', { method: 'POST' }).then(r => r.json())
      .then(j => {
        flash($('s-level'), j.ok);
        calinfo.textContent = j.ok
          ? 'volume target set: ' + j.target_db.toFixed(1) + ' dB'
          : j.error;
      })
      .catch(() => { flash($('s-level'), false); calinfo.textContent = 'request failed'; });
  });

  // ---- record remote codes (graphical picker) ----
  // Layout mirrors the main remote: each cell maps a label to a KEY_* name the
  // recorder stores. null = spacer to keep the D-pad diamond aligned.
  const REC_LAYOUT = [
    [['POWER','KEY_POWER'], ['INPUT','KEY_SOURCE']],
    [null, ['▲','KEY_UP'], null],
    [['◀','KEY_LEFT'], ['OK','KEY_OK'], ['▶','KEY_RIGHT']],
    [null, ['▼','KEY_DOWN'], null],
    [['VOL +','KEY_VOLUMEUP'], ['MUTE','KEY_MUTE'], ['CH +','KEY_CHANNELUP']],
    [['VOL −','KEY_VOLUMEDOWN'], ['MENU','KEY_MENU'], ['CH −','KEY_CHANNELDOWN']],
    [['1','KEY_1'], ['2','KEY_2'], ['3','KEY_3']],
    [['4','KEY_4'], ['5','KEY_5'], ['6','KEY_6']],
    [['7','KEY_7'], ['8','KEY_8'], ['9','KEY_9']],
    [['SETTINGS','KEY_SETTINGS'], ['0','KEY_0'], ['HOME','KEY_HOME']],
    [['BACK','KEY_BACK']],
  ];
  const STD_KEYS = new Set();
  let recSel = null;                 // {key, label, el}
  const recorded = new Set();        // KEY_* names already captured
  const customKeys = {};             // key -> label (user-added)

  function recSelect(key, label, el) {
    document.querySelectorAll('#rec-remote .rec-btn.sel, #rec-custom .rec-chip.sel')
      .forEach(b => b.classList.remove('sel'));
    if (el) el.classList.add('sel');
    recSel = { key, label };
    $('rec-sel-name').textContent = label;   // textContent: label is data, not markup
  }
  function markDone(el, key) {
    if (recorded.has(key) && !el.querySelector('.done')) {
      const d = document.createElement('span'); d.className = 'done'; el.appendChild(d);
    }
  }
  function buildRecRemote() {
    const holder = $('rec-remote'); holder.innerHTML = '';
    REC_LAYOUT.forEach(row => {
      const r = document.createElement('div');
      r.className = 'rec-row';
      r.style.gridTemplateColumns = 'repeat(' + row.length + ', 1fr)';
      row.forEach(cell => {
        const b = document.createElement('button');
        if (!cell) { b.className = 'rec-btn blank'; b.disabled = true; r.appendChild(b); return; }
        const [label, key] = cell;
        STD_KEYS.add(key);
        b.className = 'rec-btn'; b.textContent = label; b.dataset.key = key;
        b.addEventListener('click', () => recSelect(key, label, b));
        markDone(b, key);
        r.appendChild(b);
      });
      holder.appendChild(r);
    });
  }
  function addCustomChip(key, label) {
    customKeys[key] = label;
    const c = document.createElement('button');
    c.className = 'rec-chip'; c.textContent = label; c.dataset.key = key;
    c.addEventListener('click', () => recSelect(key, label, c));
    markDone(c, key);
    $('rec-custom').appendChild(c);
    return c;
  }
  function renderCustoms() {
    $('rec-custom').innerHTML = '';
    Object.keys(customKeys).forEach(k => addCustomChip(k, customKeys[k]));
  }
  buildRecRemote();

  // Load which keys are already recorded; restore custom (non-standard) ones.
  function loadRecorded() {
    fetch('/ir/recorded').then(r => r.json()).then(j => {
      recorded.clear();
      // Trust nothing from the override file: accept only well-formed KEY_
      // names (defense in depth -- the recorder enforces this server-side).
      const keys = (j.keys || []).filter(k => /^KEY_[A-Z0-9_]+$/.test(k));
      keys.forEach(k => recorded.add(k));
      keys.forEach(k => {
        if (!STD_KEYS.has(k) && !(k in customKeys))
          customKeys[k] = k.replace('KEY_', '');
      });
      buildRecRemote(); renderCustoms();
      if (recSel) {  // keep prior selection highlighted after rebuild
        const el = document.querySelector('[data-key="' + recSel.key + '"]');
        recSelect(recSel.key, recSel.label, el);
      }
    }).catch(() => {});
  }

  $('rec-add').addEventListener('click', () => {
    const label = prompt('Button name (e.g. NETFLIX, GUIDE, PIP)');
    if (label === null) return;
    const clean = label.trim().toUpperCase().replace(/[^A-Z0-9]+/g, '_')
                       .replace(/^_+|_+$/g, '');
    if (!clean) { alert('Please enter a name with letters or digits.'); return; }
    const key = 'KEY_' + clean;
    if (STD_KEYS.has(key)) {   // already on the graphic — just select it there
      const el = document.querySelector('#rec-remote [data-key="' + key + '"]');
      if (el) { recSelect(key, el.textContent, el); }
      return;
    }
    const el = (key in customKeys)
      ? document.querySelector('#rec-custom [data-key="' + key + '"]')
      : addCustomChip(key, clean);
    recSelect(key, clean, el);
  });

  $('rec-go').addEventListener('click', () => {
    if (!recSel) { $('recinfo').textContent = 'pick a button to record first'; return; }
    const k = recSel.key;
    $('recinfo').textContent = '⏺ waiting — press ' + recSel.label +
      ' on your remote NOW (9 s)…';
    fetch('/ir/record/' + encodeURIComponent(k), { method: 'POST' }).then(r => r.json())
      .then(j => {
        flash($('rec-go'), j.ok);
        if (j.ok) {
          recorded.add(j.key);
          const el = document.querySelector('[data-key="' + j.key + '"]');
          if (el) markDone(el, j.key);
          $('recinfo').textContent = '✓ ' + recSel.label + ' recorded (' +
            j.pulses + ' pulses via ' + j.device + ') — active immediately';
        } else {
          $('recinfo').textContent = j.error;
        }
      })
      .catch(() => { flash($('rec-go'), false); $('recinfo').textContent = 'request failed'; });
  });

  // ---- configuration ----
  const ENUMS = { method: ['auto','api','ir'],
                  ir_backend: ['irsend','ir-ctl','dryrun'],
                  audio_tap: ['room','upstream'],
                  mute_mode: ['mute','volume_drop','normalize'] };
  const CFG_LABELS = { tv_brand:'TV brand', tv_model:'TV model',
    method:'control method', tv_ip:'TV IP address', ir_backend:'IR backend',
    ir_device:'IR device', ir_remote:'LIRC remote name',
    audio_device:'audio device', audio_tap:'audio tap', mute_mode:'mode',
    drop_steps:'volume drop steps', max_mute_s:'max mute (s)',
    norm_tolerance_db:'norm tolerance (dB)', norm_interval_ms:'norm interval (ms)',
    norm_max_range:'norm max steps', web_port:'web port',
    acr_provider:'ACR provider', acr_host:'ACR host', acr_key:'ACR key',
    acr_secret:'ACR secret' };
  let cfgValues = {};

  function setCfg(key, value, btn) {
    fetch('/config/set?k=' + encodeURIComponent(key) +
          '&v=' + encodeURIComponent(value), { method: 'POST' })
      .then(r => r.json())
      .then(j => {
        if (!j.ok) { $('cfginfo').textContent = key + ': ' + j.error; flash(btn, false); return; }
        cfgValues[key] = value;
        btn.textContent = value === '' ? '(blank)' : value;
        if (j.restart_required) {
          btn.classList.add('dirty');
          $('cfginfo').textContent = 'saved — press RESTART ADMUFFS to apply';
        } else {
          $('cfginfo').textContent = key + ' applied live';
        }
        flash(btn, true);
      })
      .catch(() => flash(btn, false));
  }

  function loadConfig() {
    fetch('/config').then(r => r.json()).then(j => {
      cfgValues = j;
      const holder = $('cfg-rows');
      holder.innerHTML = '';
      Object.keys(CFG_LABELS).forEach(key => {
        if (!(key in j)) return;
        const row = document.createElement('div');
        row.className = 'cfg-row';
        const lbl = document.createElement('div');
        lbl.className = 'k'; lbl.textContent = CFG_LABELS[key];
        const btn = document.createElement('button');
        btn.textContent = (j[key] === '' ? '(blank)' : String(j[key]));
        btn.addEventListener('click', () => {
          if (ENUMS[key]) {
            const opts = ENUMS[key];
            const next = opts[(opts.indexOf(String(cfgValues[key])) + 1) % opts.length];
            setCfg(key, next, btn);
          } else {
            const v = prompt(CFG_LABELS[key], cfgValues[key] ?? '');
            if (v !== null) setCfg(key, v, btn);
          }
        });
        row.appendChild(lbl); row.appendChild(btn);
        holder.appendChild(row);
      });
    }).catch(() => { $('cfginfo').textContent = 'could not load config'; });
  }

  // ---- restart ----
  $('restart-btn').addEventListener('click', () => {
    if (!confirm('Restart admuffs now? (Under the systemd service it comes ' +
                 'right back; a manual run just exits.)')) return;
    fetch('/restart', { method: 'POST' }).catch(() => {});
    status.textContent = 'restarting… reload this page in a few seconds';
  });

  // ---- info ----
  // Escape everything interpolated into the info panel's HTML: the values are
  // server-controlled (OS strings, config-derived controller name), but they
  // are data, not markup.
  function esc(s) {
    return String(s).replace(/&/g,'&amp;').replace(/</g,'&lt;')
                    .replace(/>/g,'&gt;').replace(/"/g,'&quot;');
  }
  function infRow(k, v) {
    return '<div class="inf-row"><span class="k">' + esc(k) +
           '</span><span class="v">' + esc(v) + '</span></div>';
  }
  function loadInfo() {
    $('inf-rows').innerHTML = 'loading…';
    fetch('/info').then(r => r.json()).then(j => {
      let h = '';
      h += infRow('version', 'admuffs ' + j.version);
      h += infRow('hardware', j.hardware);
      h += infRow('OS', j.os);
      h += infRow('kernel', j.kernel);
      h += infRow('uptime', j.uptime);
      h += infRow('controller', j.controller);
      h += infRow('mode', j.mode + (j.automute ? '' : ' (auto-mute off)'));
      h += infRow('service', j.service);
      h += '<div class="inf-sec">P HAT SENSORS (I2C)</div>';
      if (!j.i2c) {
        h += '<div class="panel-help">' +
             esc(j.i2c_error || 'I2C bus not available — enable it in raspi-config') +
             '. (I2C never affects the IR transceiver; separate GPIO pins.)</div>';
      } else {
        h += j.htu21d
          ? infRow('HTU21D', j.htu21d.temp_c + ' °C · ' + j.htu21d.humidity_pct + ' % RH')
          : infRow('HTU21D', 'not detected');
        h += j.bh1750
          ? infRow('BH1750', j.bh1750.lux + ' lux')
          : infRow('BH1750', 'not detected');
        h += j.bmp180
          ? infRow('BMP180', j.bmp180.pressure_hpa + ' hPa · ' + j.bmp180.temp_c + ' °C')
          : infRow('BMP180', 'not detected');
      }
      $('inf-rows').innerHTML = h;
    }).catch(() => { $('inf-rows').textContent = 'could not load info'; });
  }
  $('inf-refresh').addEventListener('click', loadInfo);

  // ---- live log (poll while the panel is open) ----
  let logTimer = null, logSeq = 0;
  function pollLog() {
    fetch('/log?after=' + logSeq).then(r => r.json()).then(j => {
      if (j.lines.length) {
        const box = $('logbox');
        const stick = box.scrollTop + box.clientHeight >= box.scrollHeight - 20;
        j.lines.forEach(l => {
          const d = document.createElement('div');
          d.textContent = l;
          if (l.indexOf(' WARN ') > 0) d.className = 'warn';
          if (l.indexOf(' ERROR ') > 0) d.className = 'err';
          box.appendChild(d);
        });
        while (box.childNodes.length > 400) box.removeChild(box.firstChild);
        if (stick) box.scrollTop = box.scrollHeight;
        logSeq = j.next;
      }
    }).catch(() => {});
  }
  function startLog() {
    if (logTimer) return;
    $('logbox').innerHTML = ''; logSeq = 0;
    pollLog();
    logTimer = setInterval(pollLog, 1000);
  }
  function stopLog() {
    if (logTimer) { clearInterval(logTimer); logTimer = null; }
  }

  function loadSvcHint() {
    fetch('/info').then(r => r.json()).then(j => {
      $('svc-hint').textContent =
        (j.service === 'active')
          ? 'running as a systemd service — survives reboots'
          : (j.service === 'unsupported')
            ? 'running manually (service install is Linux-only)'
            : 'not running as a service: run  sudo admuffs --install-service  on ' +
              'the Pi so admuffs starts on boot and survives restarts';
    }).catch(() => {});
  }
</script>
</body>
</html>
)HTML";

std::string http_response(int code, const char* status, const std::string& ctype,
                          const std::string& body,
                          const std::string& extra_headers = "") {
    std::string r = "HTTP/1.1 " + std::to_string(code) + " " + status + "\r\n";
    r += "Content-Type: " + ctype + "\r\n";
    r += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    r += "Cache-Control: no-store\r\n";
    // OWASP security headers. CSP allows only same-origin inline styles/scripts
    // (the whole app is inlined); no external loads, no framing, no sniffing.
    r += "Content-Security-Policy: default-src 'none'; style-src 'unsafe-inline'; "
         "script-src 'unsafe-inline'; connect-src 'self'; img-src 'self' data:; "
         "base-uri 'none'; form-action 'none'; frame-ancestors 'none'\r\n";
    r += "X-Content-Type-Options: nosniff\r\n";
    r += "X-Frame-Options: DENY\r\n";
    r += "Referrer-Policy: no-referrer\r\n";
    r += extra_headers;
    r += "Connection: close\r\n\r\n";
    r += body;
    return r;
}

std::string url_decode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            out += (char)strtol(s.substr(i + 1, 2).c_str(), nullptr, 16);
            i += 2;
        } else if (s[i] == '+') out += ' ';
        else out += s[i];
    }
    return out;
}

// "k=a&v=b" -> {k:a, v:b}, values URL-decoded
std::map<std::string, std::string> parse_query(const std::string& q) {
    std::map<std::string, std::string> out;
    for (const auto& pair : split(q, '&')) {
        size_t eq = pair.find('=');
        if (eq == std::string::npos) continue;
        out[url_decode(pair.substr(0, eq))] = url_decode(pair.substr(eq + 1));
    }
    return out;
}

// Extract the admuffs_session token from a Cookie header line, if present.
std::string cookie_token(const std::string& req) {
    std::string low = to_lower(req);
    size_t p = low.find("cookie:");
    if (p == std::string::npos) return "";
    size_t eol = req.find("\r\n", p);
    std::string line = req.substr(p, (eol == std::string::npos ? req.size() : eol) - p);
    size_t k = line.find("admuffs_session=");
    if (k == std::string::npos) return "";
    k += strlen("admuffs_session=");
    size_t end = line.find_first_of("; \r\n", k);
    return line.substr(k, (end == std::string::npos ? line.size() : end) - k);
}
}  // namespace

bool WebServer::key_from_name(const std::string& name, TvCommand& out) {
    static const std::map<std::string, TvCommand> kMap = {
        {"Power", TvCommand::PowerToggle}, {"Input", TvCommand::Input},
        {"Up", TvCommand::Up}, {"Down", TvCommand::Down},
        {"Left", TvCommand::Left}, {"Right", TvCommand::Right},
        {"Ok", TvCommand::Ok},
        {"Back", TvCommand::Back}, {"Settings", TvCommand::Settings},
        {"Home", TvCommand::Home},
        {"VolumeUp", TvCommand::VolumeUp}, {"VolumeDown", TvCommand::VolumeDown},
        {"Mute", TvCommand::ToggleMute},
        {"ChannelUp", TvCommand::ChannelUp}, {"ChannelDown", TvCommand::ChannelDown},
    };
    auto it = kMap.find(name);
    if (it != kMap.end()) { out = it->second; return true; }
    if (name.size() == 1 && name[0] >= '0' && name[0] <= '9') {
        out = static_cast<TvCommand>(static_cast<int>(TvCommand::Digit0) + (name[0] - '0'));
        return true;
    }
    return false;
}

WebServer::WebServer(int port, WebHooks hooks)
    : port_(port), hooks_(std::move(hooks)) {}

WebServer::~WebServer() { stop(); }

bool WebServer::start() {
    listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) return false;
    int one = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port_);
    if (bind(listen_fd_, (sockaddr*)&addr, sizeof(addr)) < 0 ||
        listen(listen_fd_, 8) < 0) {
        LOG_WARN("web remote: cannot bind port %d (%s)", port_, strerror(errno));
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }
    running_ = true;
    thread_ = std::thread(&WebServer::loop, this);
    LOG_INFO("web remote: http://<pi-address>:%d/", port_);
    return true;
}

void WebServer::stop() {
    running_ = false;
    if (listen_fd_ >= 0) {
        shutdown(listen_fd_, SHUT_RDWR);
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
    if (thread_.joinable()) thread_.join();
}

void WebServer::loop() {
    while (running_) {
        struct sockaddr_in peer{};
        socklen_t plen = sizeof(peer);
        int fd = accept(listen_fd_, (sockaddr*)&peer, &plen);
        if (fd < 0) {
            if (running_) sleep_ms(50);
            continue;
        }
        // Per-recv timeout is short so a stalled client can't pin the single
        // server thread (handle_client also enforces an overall read deadline);
        // the send timeout stays generous for large responses on slow links.
        struct timeval rcv{5, 0}, snd{15, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcv, sizeof(rcv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &snd, sizeof(snd));
        handle_client(fd);
        ::close(fd);
    }
}

void WebServer::handle_client(int fd) {
    std::string req;
    char buf[2048];
    // Overall deadline across the whole request read: a client that dribbles
    // bytes (or never sends the header terminator) is dropped instead of
    // holding the single-threaded server open indefinitely (slowloris).
    const time_t start = time(nullptr);
    const int kReadDeadlineS = 10;
    // Read until end of headers; keep any body bytes that arrived with them.
    while (req.find("\r\n\r\n") == std::string::npos && req.size() < 16384) {
        if (time(nullptr) - start > kReadDeadlineS) break;
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        req.append(buf, (size_t)n);
    }
    size_t hdr_end = req.find("\r\n\r\n");
    size_t eol = req.find("\r\n");
    if (eol == std::string::npos) return;
    std::vector<std::string> parts = split(req.substr(0, eol), ' ');
    if (parts.size() < 2) return;
    const std::string& method = parts[0];
    std::string path = parts[1];
    std::string query;
    size_t qm = path.find('?');
    if (qm != std::string::npos) { query = path.substr(qm + 1); path = path.substr(0, qm); }

    // Read the request body if any (Content-Length, capped). Used for the PIN
    // endpoints so the secret is never in the URL / access logs.
    std::string body;
    if (hdr_end != std::string::npos) {
        body = req.substr(hdr_end + 4);
        std::string low = to_lower(req.substr(0, hdr_end));
        size_t cl = low.find("content-length:");
        if (cl != std::string::npos) {
            long want = atol(req.c_str() + cl + strlen("content-length:"));
            if (want < 0) want = 0;
            if (want > 4096) want = 4096;   // PIN bodies are tiny; cap hard
            while ((long)body.size() < want) {
                if (time(nullptr) - start > kReadDeadlineS) break;
                ssize_t n = recv(fd, buf, sizeof(buf), 0);
                if (n <= 0) break;
                body.append(buf, (size_t)n);
                if (body.size() > 4096) break;
            }
            if ((long)body.size() > want) body.resize(want);
        }
    }

    const std::string token = cookie_token(req);
    const bool authed = hooks_.auth_valid && hooks_.auth_valid(token);

    // Proper JSON string escaper: quotes, backslash, and ALL control chars
    // (so a newline/NUL smuggled into a logged value or reflected error can't
    // break the JSON body or forge/split a line in the /log viewer).
    auto json_esc = [](const std::string& s) {
        static const char* hex = "0123456789abcdef";
        std::string e;
        for (unsigned char c : s) {
            switch (c) {
                case '"':  e += "\\\""; break;
                case '\\': e += "\\\\"; break;
                case '\n': e += "\\n";  break;
                case '\r': e += "\\r";  break;
                case '\t': e += "\\t";  break;
                default:
                    if (c < 0x20) { e += "\\u00"; e += hex[c >> 4]; e += hex[c & 0xF]; }
                    else e += (char)c;
            }
        }
        return e;
    };
    auto not_wired = [&]() {
        return http_response(200, "OK", "application/json",
                             "{\"ok\":false,\"error\":\"not wired\"}");
    };

    // Endpoints reachable WITHOUT a session: the page shell (static, no data)
    // and the login endpoint. Everything else requires authentication.
    const bool is_public = (method == "GET" && (path == "/" || path == "/index.html")) ||
                           (method == "POST" && path == "/auth");
    if (!is_public && !authed) {
        std::string r = http_response(401, "Unauthorized", "application/json",
                                      "{\"ok\":false,\"error\":\"authentication required\"}");
        size_t s2 = 0;
        while (s2 < r.size()) { ssize_t n = ::send(fd, r.data()+s2, r.size()-s2, 0); if (n<=0) break; s2 += (size_t)n; }
        return;
    }

    std::string resp;
    bool restart_after = false;

    if (method == "POST" && path == "/auth") {
        // PIN comes in the body as pin=NNNN (form-encoded).
        auto form = parse_query(body);
        std::string pin = form.count("pin") ? form["pin"] : "";
        int retry = 0;
        std::string tok = hooks_.auth_login ? hooks_.auth_login(pin, retry) : "";
        if (!tok.empty()) {
            // HttpOnly + SameSite=Strict; no Secure flag (LAN HTTP, no TLS).
            std::string set = "Set-Cookie: admuffs_session=" + tok +
                              "; HttpOnly; SameSite=Strict; Path=/; Max-Age=43200\r\n";
            resp = http_response(200, "OK", "application/json", "{\"ok\":true}", set);
        } else {
            std::string b = retry > 0
                ? "{\"ok\":false,\"locked\":true,\"retry_after\":" + std::to_string(retry) + "}"
                : "{\"ok\":false}";
            resp = http_response(retry > 0 ? 429 : 401,
                                 retry > 0 ? "Too Many Requests" : "Unauthorized",
                                 "application/json", b);
        }

    } else if (method == "POST" && path == "/logout") {
        if (hooks_.auth_logout) hooks_.auth_logout(token);
        std::string clr = "Set-Cookie: admuffs_session=; HttpOnly; SameSite=Strict; "
                          "Path=/; Max-Age=0\r\n";
        resp = http_response(200, "OK", "application/json", "{\"ok\":true}", clr);

    } else if (method == "POST" && path == "/pin") {
        if (!hooks_.auth_change) resp = not_wired();
        else {
            auto form = parse_query(body);
            std::string err = hooks_.auth_change(form.count("current") ? form["current"] : "",
                                                 form.count("new") ? form["new"] : "");
            resp = err.empty()
                ? http_response(200, "OK", "application/json", "{\"ok\":true}")
                : http_response(200, "OK", "application/json",
                                "{\"ok\":false,\"error\":\"" + json_esc(err) + "\"}");
        }

    } else if (method == "GET" && (path == "/" || path == "/index.html")) {
        resp = http_response(200, "OK", "text/html; charset=utf-8", kRemotePage);

    } else if (method == "GET" && path == "/status") {
        std::string name = hooks_.status ? hooks_.status() : "unknown";
        bool am = hooks_.automute_get ? hooks_.automute_get() : true;
        resp = http_response(200, "OK", "application/json",
                             "{\"controller\":\"" + json_esc(name) + "\",\"automute\":" +
                             (am ? "true" : "false") + "}");

    } else if (method == "POST" && path == "/automute") {
        if (!hooks_.automute_get || !hooks_.automute_set) resp = not_wired();
        else {
            bool now = hooks_.automute_set(!hooks_.automute_get());
            LOG_INFO("web remote: auto-mute toggled -> %s", now ? "ON" : "OFF");
            resp = http_response(200, "OK", "application/json",
                                 std::string("{\"automute\":") + (now ? "true" : "false") + "}");
        }

    } else if (method == "POST" && path == "/sample/level") {
        if (!hooks_.norm_target) resp = not_wired();
        else {
            AudioSampleResult r = hooks_.norm_target();
            char body[256];
            if (r.ok) {
                snprintf(body, sizeof(body),
                         "{\"ok\":true,\"label\":\"level\",\"target_db\":%.1f}", r.db);
                LOG_INFO("web remote: volume target set (%.1f dB)", r.db);
            } else {
                snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"%s\"}",
                         json_esc(r.error).c_str());
            }
            resp = http_response(200, "OK", "application/json", body);
        }

    } else if (method == "POST" &&
               (path == "/sample/normal" || path == "/sample/commercial")) {
        if (!hooks_.sample) resp = not_wired();
        else {
            bool commercial = (path == "/sample/commercial");
            AudioSampleResult r = hooks_.sample(commercial);
            char body[320];
            if (r.ok) {
                snprintf(body, sizeof(body),
                         "{\"ok\":true,\"label\":\"%s\",\"db\":%.1f,\"avg_db\":%.1f,"
                         "\"n\":%d,\"have_both\":%s,\"delta_db\":%.1f}",
                         commercial ? "commercial" : "normal", r.db, r.avg_db,
                         r.n, r.have_both ? "true" : "false", r.derived_delta_db);
            } else {
                snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"%s\"}",
                         json_esc(r.error).c_str());
            }
            resp = http_response(200, "OK", "application/json", body);
        }

    } else if (method == "GET" && path == "/config") {
        resp = hooks_.config_get
            ? http_response(200, "OK", "application/json", hooks_.config_get())
            : not_wired();

    } else if (method == "POST" && path == "/config/set") {
        if (!hooks_.config_set) resp = not_wired();
        else {
            auto q = parse_query(query);
            if (!q.count("k"))
                resp = http_response(200, "OK", "application/json",
                                     "{\"ok\":false,\"error\":\"missing k parameter\"}");
            else
                resp = http_response(200, "OK", "application/json",
                                     hooks_.config_set(q["k"], q.count("v") ? q["v"] : ""));
        }

    } else if (method == "GET" && path == "/ir/recorded") {
        resp = hooks_.recorded_keys
            ? http_response(200, "OK", "application/json", hooks_.recorded_keys())
            : http_response(200, "OK", "application/json", "{\"keys\":[]}");

    } else if (method == "POST" && path.rfind("/ir/record/", 0) == 0) {
        if (!hooks_.record_key) resp = not_wired();
        else {
            std::string key = url_decode(path.substr(strlen("/ir/record/")));
            // Log a sanitized copy only: the key is attacker-controlled until
            // record_key() validates it, so strip anything outside the KEY_
            // charset before it reaches the log ring / file.
            std::string safe_key;
            for (char c : key)
                if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')
                    safe_key += c;
            if (safe_key.size() > 40) safe_key.resize(40);
            LOG_INFO("web remote: recording %s from the IR receiver...",
                     safe_key.empty() ? "(invalid key)" : safe_key.c_str());
            resp = http_response(200, "OK", "application/json", hooks_.record_key(key));
        }

    } else if (method == "GET" && path == "/log") {
        auto q = parse_query(query);
        uint64_t after = q.count("after") ? strtoull(q["after"].c_str(), nullptr, 10) : 0;
        auto lines = log_since(after, 200);
        std::string body = "{\"lines\":[";
        uint64_t last = after;
        for (size_t i = 0; i < lines.size(); ++i) {
            if (i) body += ",";
            body += "\"" + json_esc(lines[i].text) + "\"";
            last = lines[i].seq;
        }
        body += "],\"next\":" + std::to_string(last) + "}";
        resp = http_response(200, "OK", "application/json", body);

    } else if (method == "GET" && path == "/info") {
        resp = hooks_.info
            ? http_response(200, "OK", "application/json", hooks_.info())
            : not_wired();

    } else if (method == "POST" && path == "/restart") {
        resp = http_response(200, "OK", "application/json", "{\"ok\":true}");
        restart_after = hooks_.restart != nullptr;

    } else if (method == "POST" && path.rfind("/key/", 0) == 0) {
        std::string key = url_decode(path.substr(5));
        TvCommand cmd;
        if (!key_from_name(key, cmd)) {
            resp = http_response(404, "Not Found", "application/json",
                                 "{\"ok\":false,\"error\":\"unknown key\"}");
        } else {
            bool ok = hooks_.send ? hooks_.send(cmd) : false;
            LOG_INFO("web remote: %s -> %s", tv_command_name(cmd), ok ? "sent" : "FAILED");
            resp = http_response(200, "OK", "application/json",
                                 std::string("{\"ok\":") + (ok ? "true" : "false") + "}");
        }
    } else {
        resp = http_response(404, "Not Found", "text/plain", "not found\n");
    }

    size_t sent = 0;
    while (sent < resp.size()) {
        ssize_t n = ::send(fd, resp.data() + sent, resp.size() - sent, 0);
        if (n <= 0) break;
        sent += (size_t)n;
    }
    // Fire the restart only after the response has gone out.
    if (restart_after) {
        LOG_INFO("web remote: restart requested");
        hooks_.restart();
    }
}

}  // namespace admuffs
