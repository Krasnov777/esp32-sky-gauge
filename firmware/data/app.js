// ESP Sky Gauge — single-file controller for the web UI.
// Drives a WebSocket to /ws: receives {type:"state"|"radar"}, sends {type:"config"|"hello"|"command"}.
(() => {
  'use strict';

  // ── DOM refs ───────────────────────────────────────────────────────────────
  const $  = (id) => document.getElementById(id);
  const el = {
    conn: $('conn'),
    brightness: $('brightness'), brightnessVal: $('brightnessVal'),
    radarLat: $('radarLat'), radarLon: $('radarLon'), radarRange: $('radarRange'),
    radarPoll: $('radarPoll'), radarTags: $('radarTags'),
    radarTheme: $('radarTheme'), radarAlert: $('radarAlert'), radarAuto: $('radarAuto'),
    radarAutoBase: $('radarAutoBase'),
    radarSaveBtn: $('radarSaveBtn'), radarStatus: $('radarStatus'),
    radarLiveBlock: $('radarLiveBlock'), radarCanvas: $('radarCanvas'),
    radarCanvasHint: $('radarCanvasHint'),
    haUrl: $('haUrl'), haToken: $('haToken'), haPoll: $('haPoll'),
    haTiles: $('haTiles'), haSaveBtn: $('haSaveBtn'), haStatus: $('haStatus'),
    ssid: $('ssid'), password: $('password'), hostname: $('hostname'),
    wifiSaveBtn: $('wifiSaveBtn'), wifiStatus: $('wifiStatus'),
    rebootBtn: $('rebootBtn'), resetBtn: $('resetBtn'),
    shotBtn: $('shotBtn'), shotStatus: $('shotStatus'),
    hostFooter: $('hostFooter'),
    modeBtns: document.querySelectorAll('.mode-btn'),
  };

  const HA_TYPES = ['temperature', 'climate', 'humidity', 'power', 'battery',
                    'co2', 'pressure', 'voltage', 'custom'];
  const HA_ICONS = ['auto', 'thermometer', 'humidity', 'power', 'battery', 'sun',
                    'home', 'gauge', 'fire', 'snow', 'bulb'];
  const HA_TILES = 5;
  // Build the tile config rows once (one entity per page).
  for (let i = 0; i < HA_TILES; i++) {
    const row = document.createElement('div');
    row.className = 'grid2';
    row.style.cssText = 'margin-top:10px;padding-top:10px;border-top:1px solid var(--line)';
    row.innerHTML =
      `<label>Page ${i + 1} label <input type="text" id="haLbl${i}" placeholder="Living room"></label>` +
      `<label>Type <select id="haTyp${i}">` +
        HA_TYPES.map(t => `<option value="${t}">${t}</option>`).join('') +
      `</select></label>` +
      `<label>Icon <select id="haIco${i}">` +
        HA_ICONS.map(t => `<option value="${t}">${t}</option>`).join('') +
      `</select></label>` +
      `<label>Entity <input type="text" id="haEnt${i}" placeholder="sensor.living_temperature" autocomplete="off"></label>` +
      `<label>Secondary entity (optional) <input type="text" id="haEn2${i}" placeholder="sensor.living_humidity" autocomplete="off"></label>`;
    el.haTiles.appendChild(row);
  }

  let ws = null;
  let backoffMs = 500;
  let suppressSend = false;          // true while applying server state to inputs

  let radarData = null, radarRxTime = 0, radarTheme = 0, sweepDeg = 0, lastFrame = 0;

  // ── Per-mode card visibility (tabs) ──────────────────────────────────────
  const MODE_CARDS = {
    0: ['radarLiveBlock', 'radarBlock'],
    1: ['weatherBlock'],
    2: ['radarLiveBlock', 'radarBlock', 'weatherBlock'],   // Auto uses both
    3: ['homeBlock'],
  };
  const ALL_MODE_CARDS = ['radarLiveBlock', 'radarBlock', 'weatherBlock', 'homeBlock'];

  function showCardsFor(mode) {
    const show = new Set(MODE_CARDS[mode] || []);
    for (const id of ALL_MODE_CARDS) $(id).hidden = !show.has(id);
  }

  // ── WebSocket plumbing ────────────────────────────────────────────────────
  function connect() {
    const proto = location.protocol === 'https:' ? 'wss:' : 'ws:';
    ws = new WebSocket(`${proto}//${location.host}/ws`);

    ws.addEventListener('open', () => {
      backoffMs = 500;
      el.conn.classList.remove('offline'); el.conn.classList.add('online');
      ws.send(JSON.stringify({ type: 'hello' }));
    });
    ws.addEventListener('close', () => {
      el.conn.classList.remove('online'); el.conn.classList.add('offline');
      setTimeout(connect, backoffMs);
      backoffMs = Math.min(backoffMs * 2, 5000);
    });
    ws.addEventListener('error', () => ws.close());
    ws.addEventListener('message', (ev) => {
      let msg;
      try { msg = JSON.parse(ev.data); } catch { return; }
      if (msg.type === 'state' && msg.data) applyState(msg.data);
      else if (msg.type === 'radar') { radarData = msg; radarRxTime = performance.now(); }
    });
  }

  function sendConfig(patch) {
    if (suppressSend) return;
    if (!ws || ws.readyState !== WebSocket.OPEN) return;
    ws.send(JSON.stringify({ type: 'config', patch }));
  }

  // ── State sync ────────────────────────────────────────────────────────────
  function applyState(s) {
    suppressSend = true;

    el.brightness.value = s.brightness ?? 200;
    el.brightnessVal.value = el.brightness.value;

    // Radar: 0/0 means "never configured" — leave the inputs blank then.
    const rLat = s.radar?.lat ?? 0, rLon = s.radar?.lon ?? 0;
    el.radarLat.value   = (rLat === 0 && rLon === 0) ? '' : rLat.toFixed(4);
    el.radarLon.value   = (rLat === 0 && rLon === 0) ? '' : rLon.toFixed(4);
    el.radarRange.value = s.radar?.range_km ?? 100;
    el.radarPoll.value  = s.radar?.poll_s ?? 10;
    el.radarTags.checked = s.radar?.show_tags ?? true;
    el.radarTheme.value  = String(s.radar?.theme ?? 0);
    el.radarAlert.value  = s.radar?.alert_km ?? 3;
    el.radarAuto.value   = s.radar?.auto_km ?? 5;
    el.radarAutoBase.value = String(s.radar?.auto_base ?? 0);
    radarTheme = s.radar?.theme ?? 0;

    // Home Assistant
    el.haUrl.value  = s.home?.url ?? '';
    el.haPoll.value = s.home?.poll_s ?? 15;
    el.haToken.placeholder = s.home?.token_set ? '(leave blank to keep current)' : 'long-lived access token';
    const tiles = s.home?.tiles ?? [];
    for (let i = 0; i < HA_TILES; i++) {
      const t = tiles[i] ?? {};
      $('haLbl' + i).value = t.label ?? '';
      $('haTyp' + i).value = t.type ?? 'temperature';
      $('haIco' + i).value = t.icon ?? 'auto';
      $('haEnt' + i).value = t.entity ?? '';
      $('haEn2' + i).value = t.entity2 ?? '';
    }

    el.ssid.value     = s.wifi?.ssid     ?? '';
    el.hostname.value = s.wifi?.hostname ?? '';
    el.password.placeholder = (s.wifi?.password || '') ? '(leave blank to keep)' : '';

    el.hostFooter.textContent = (s.wifi?.hostname || 'esp-gauge') + '.local';

    const mode = s.mode ?? 0;
    el.modeBtns.forEach(btn => {
      btn.classList.toggle('active', Number(btn.dataset.mode) === mode);
    });
    showCardsFor(mode);

    suppressSend = false;
  }

  // ── Input wiring ──────────────────────────────────────────────────────────
  function wireInputs() {
    el.modeBtns.forEach(btn => {
      btn.addEventListener('click', () => {
        const m = Number(btn.dataset.mode);
        el.modeBtns.forEach(b => b.classList.toggle('active', b === btn));
        showCardsFor(m);
        sendConfig({ mode: m });
      });
    });

    el.brightness.addEventListener('input', () => {
      el.brightnessVal.value = el.brightness.value;
      sendConfig({ brightness: Number(el.brightness.value) });
    });

    // Radar: explicit save — lat/lon/range belong together, auto-committing
    // half-typed coordinates would point the scope at the wrong place.
    el.radarSaveBtn.addEventListener('click', () => {
      const lat = parseFloat(el.radarLat.value);
      const lon = parseFloat(el.radarLon.value);
      if (!Number.isFinite(lat) || !Number.isFinite(lon) ||
          Math.abs(lat) > 90 || Math.abs(lon) > 180) {
        el.radarStatus.textContent = 'Enter a valid latitude and longitude first.';
        el.radarLat.focus();
        return;
      }
      sendConfig({ radar: {
        lat, lon,
        range_km: Number(el.radarRange.value) || 100,
        poll_s:   Number(el.radarPoll.value)  || 10,
        show_tags: el.radarTags.checked,
        theme:    Number(el.radarTheme.value) || 0,
        alert_km: Math.max(0, Number(el.radarAlert.value) || 0),
        auto_km:  Math.max(0, Number(el.radarAuto.value) || 0),
        auto_base: Number(el.radarAutoBase.value) || 0,
      }});
      el.radarStatus.textContent = 'Saved.';
      setTimeout(() => { el.radarStatus.textContent = ''; }, 3000);
    });

    // Home Assistant: explicit save (URL + token + tiles belong together).
    el.haSaveBtn.addEventListener('click', () => {
      const home = {
        url: el.haUrl.value.trim(),
        poll_s: Number(el.haPoll.value) || 15,
        tiles: [],
      };
      if (el.haToken.value) home.token = el.haToken.value;   // blank = keep current
      for (let i = 0; i < HA_TILES; i++) {
        home.tiles.push({
          label:   $('haLbl' + i).value.trim(),
          type:    $('haTyp' + i).value,
          icon:    $('haIco' + i).value,
          entity:  $('haEnt' + i).value.trim(),
          entity2: $('haEn2' + i).value.trim(),
        });
      }
      sendConfig({ home });
      el.haToken.value = '';   // don't keep the secret in the field
      el.haStatus.textContent = 'Saved.';
      setTimeout(() => { el.haStatus.textContent = ''; }, 3000);
    });

    // WiFi: explicit save (it needs a reboot, so no live auto-commit).
    el.wifiSaveBtn.addEventListener('click', () => {
      const ssid = el.ssid.value.trim();
      if (!ssid) {
        el.wifiStatus.textContent = 'Enter an SSID first.';
        el.ssid.focus();
        return;
      }
      const patch = { wifi: { ssid } };
      if (el.password.value) patch.wifi.password = el.password.value;
      if (el.hostname.value.trim()) patch.wifi.hostname = el.hostname.value.trim();

      sendConfig(patch);
      el.wifiStatus.textContent = `Saved. Rebooting to join "${ssid}"…`;
      el.wifiSaveBtn.disabled = true;
      // Give the device a moment to persist to NVS, then reboot it.
      setTimeout(() => {
        ws?.send(JSON.stringify({ type: 'command', cmd: 'reboot' }));
      }, 700);
    });

    el.shotBtn.addEventListener('click', async () => {
      el.shotStatus.textContent = 'Capturing…';
      try {
        await fetch('/api/shot', { method: 'POST' });
        await new Promise(r => setTimeout(r, 600));   // capture happens on the device loop
        window.open('/shot.bmp?t=' + Date.now(), '_blank');
        el.shotStatus.textContent = '';
      } catch {
        el.shotStatus.textContent = 'Capture failed.';
      }
    });

    el.rebootBtn.addEventListener('click', async () => {
      if (!confirm('Reboot the device?')) return;
      ws?.send(JSON.stringify({ type: 'command', cmd: 'reboot' }));
    });
    el.resetBtn.addEventListener('click', async () => {
      if (!confirm('Erase all settings and reboot?')) return;
      ws?.send(JSON.stringify({ type: 'command', cmd: 'factory_reset' }));
    });
  }

  // ── Live radar scope (canvas mirror of the device screen) ────────────────
  // Device pushes {type:"radar", ac:[{cs,rt,x,y,alt,gs,trk,vr,gnd,emg}], age, range}
  // every 2 s; we dead-reckon positions between pushes and draw at ~30 fps.
  function drawRadar(now) {
    requestAnimationFrame(drawRadar);
    if (el.radarLiveBlock.hidden) return;
    if (now - lastFrame < 33) return;
    const dtFrame = lastFrame ? now - lastFrame : 33;
    lastFrame = now;

    const ctx = el.radarCanvas.getContext('2d');
    const W = el.radarCanvas.width, C = W / 2, R = C - 8;
    const bright = radarTheme === 1 ? '#ffb000' : '#00ff46';
    const dim    = radarTheme === 1 ? 'rgba(255,176,0,.35)' : 'rgba(0,255,70,.35)';

    ctx.clearRect(0, 0, W, W);

    // rings at round distances + crosshair + cardinal ticks
    const rangeKm = radarData?.range || 100;
    let ringStep = 0, ringFb = 0;
    for (const s of [200, 100, 50, 25, 10, 5, 2, 1]) {
      const c = Math.floor((rangeKm - 0.01) / s);
      if (c >= 2 && c <= 3) { ringStep = s; break; }
      if (c === 1 && !ringFb) ringFb = s;
    }
    ringStep = ringStep || ringFb || rangeKm;
    ctx.strokeStyle = dim; ctx.lineWidth = 1;
    ctx.beginPath(); ctx.arc(C, C, R, 0, 7); ctx.stroke();
    for (let d = ringStep; d < rangeKm; d += ringStep) {
      ctx.beginPath(); ctx.arc(C, C, R * d / rangeKm, 0, 7); ctx.stroke();
    }
    ctx.beginPath();
    ctx.moveTo(C - R, C); ctx.lineTo(C + R, C);
    ctx.moveTo(C, C - R); ctx.lineTo(C, C + R);
    ctx.stroke();
    ctx.fillStyle = dim; ctx.font = '11px sans-serif'; ctx.textAlign = 'center';
    ctx.fillText('N', C, 16);

    // sweep: rotating gradient wedge (8 s / revolution, matches the device)
    sweepDeg = (sweepDeg + dtFrame * 0.045) % 360;
    const a0 = (sweepDeg - 90) * Math.PI / 180;
    if (ctx.createConicGradient) {
      const g = ctx.createConicGradient(a0, C, C);
      g.addColorStop(0, radarTheme === 1 ? 'rgba(255,176,0,.5)' : 'rgba(0,255,70,.5)');
      g.addColorStop(0.12, 'rgba(0,0,0,0)');
      ctx.fillStyle = g;
      ctx.beginPath(); ctx.moveTo(C, C); ctx.arc(C, C, R, a0 - 0.8, a0); ctx.closePath();
      ctx.fill();
    }
    ctx.strokeStyle = bright; ctx.lineWidth = 2;
    ctx.beginPath(); ctx.moveTo(C, C);
    ctx.lineTo(C + R * Math.cos(a0), C + R * Math.sin(a0)); ctx.stroke();
    // center hub over the sweep origin
    ctx.fillStyle = bright;
    ctx.beginPath(); ctx.arc(C, C, 6, 0, 7); ctx.fill();

    if (!radarData) { el.radarCanvasHint.textContent = 'Waiting for data…'; return; }
    const range = radarData.range || 100;
    const ageMs = Math.min((radarData.age ?? 0) + (now - radarRxTime), 60000);
    const dtH = ageMs / 3.6e6;

    // range marks on the east crosshair arm, matching the device scope
    ctx.fillStyle = dim; ctx.font = '10px sans-serif'; ctx.textAlign = 'right';
    for (let d = ringStep; d < range; d += ringStep)
      ctx.fillText(String(d), C + R * d / range - 3, C - 5);
    ctx.fillText(range + 'km', C + R - 3, C - 5);

    for (const a of radarData.ac || []) {
      const trk = (a.trk || 0) * Math.PI / 180;
      const v = (a.gnd ? 0 : a.gs || 0) * 1.852;
      const exKm = a.x + v * dtH * Math.sin(trk), eyKm = a.y + v * dtH * Math.cos(trk);
      let px = exKm / range * R, py = -eyKm / range * R;
      const r = Math.hypot(px, py);
      if (r > R) { px *= R / r; py *= R / r; }
      const x = C + px, y = C + py;
      const col = a.emg ? '#ff281e' : bright;

      if (!a.gnd && (a.gs || 0) > 30) {           // heading vector
        const len = 8 + Math.min(a.gs, 480) / 35;
        ctx.strokeStyle = col; ctx.lineWidth = 2;
        ctx.beginPath(); ctx.moveTo(x, y);
        ctx.lineTo(x + len * Math.sin(trk), y - len * Math.cos(trk)); ctx.stroke();
      }
      ctx.fillStyle = col;
      ctx.beginPath(); ctx.arc(x, y, 3.5, 0, 7); ctx.fill();
      if (a.cs) {
        ctx.font = '11px sans-serif';
        ctx.textAlign = x > C ? 'right' : 'left';
        ctx.fillText(a.cs, x > C ? x - 7 : x + 7, y - 6);
        if (!a.gnd) {
          const vs = a.vr > 300 ? '↑' : a.vr < -300 ? '↓' : '';
          ctx.fillStyle = dim;
          ctx.fillText(`${Math.round(a.alt / 100)}${vs}`, x > C ? x - 7 : x + 7, y + 14);
        }
      }
    }
    el.radarCanvasHint.textContent =
      `${(radarData.ac || []).length} aircraft · ${range} km · labels show callsign + FL`;
  }
  requestAnimationFrame(drawRadar);

  wireInputs();
  connect();
})();
