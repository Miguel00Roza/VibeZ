#pragma once

#include <string_view>

namespace vibez::server {

// Single-page viewer. It is embedded in the server binary so the web UI can be
// served from any folder without extra files, and it uses only browser APIs so
// it works on desktop and mobile without installing anything.
inline constexpr std::string_view kWebPageHtml = R"HTML(<!doctype html>
<html lang="pt-BR">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
<meta name="theme-color" content="#16181d">
<title>VibeZ · Tela ao vivo</title>
<style>
  :root {
    color-scheme: dark;
    --bg: #16181d;
    --panel: #1f232b;
    --line: #2c313b;
    --text: #e6e8ec;
    --muted: #9aa3b2;
    --accent: #4ade80;
  }
  * { box-sizing: border-box; }
  html, body { height: 100%; }
  body {
    margin: 0;
    background: var(--bg);
    color: var(--text);
    font-family: "Segoe UI", system-ui, -apple-system, sans-serif;
    display: flex;
    flex-direction: column;
    height: 100dvh;
    overflow: hidden;
    -webkit-tap-highlight-color: transparent;
  }
  header {
    display: flex;
    align-items: center;
    gap: 12px;
    padding: 10px 16px;
    background: var(--panel);
    border-bottom: 1px solid var(--line);
    flex-wrap: wrap;
  }
  .brand { font-weight: 700; letter-spacing: .3px; white-space: nowrap; }
  .brand strong { color: var(--accent); }
  #status {
    display: flex;
    align-items: center;
    gap: 8px;
    font-size: 13px;
    color: var(--muted);
    margin-left: auto;
    white-space: nowrap;
  }
  .dot { width: 9px; height: 9px; border-radius: 50%; background: #6b7280; }
  .dot.live { background: var(--accent); box-shadow: 0 0 8px var(--accent); animation: pulse 2s infinite; }
  @keyframes pulse { 50% { opacity: .35; } }
  select, button, a.button {
    font: inherit;
    color: var(--text);
    background: #262b34;
    border: 1px solid #343b47;
    border-radius: 10px;
    padding: 9px 12px;
    min-height: 40px;
    cursor: pointer;
  }
  select { max-width: 46vw; }
  button:active, a.button:active { transform: scale(.98); }
  a.button { text-decoration: none; display: inline-flex; align-items: center; justify-content: center; }
  main {
    position: relative;
    flex: 1;
    min-height: 0;
    background: #000;
    display: flex;
    align-items: center;
    justify-content: center;
  }
  #screen {
    display: none;
    max-width: 100%;
    max-height: 100%;
    object-fit: contain;
    user-select: none;
    -webkit-user-select: none;
  }
  #screen.visible { display: block; }
  #placeholder {
    text-align: center;
    padding: 24px;
    color: var(--muted);
    max-width: 520px;
  }
  #placeholder h1 {
    color: var(--text);
    font-size: clamp(18px, 4vw, 23px);
    font-weight: 600;
    margin: .8em 0 .3em;
  }
  #placeholder p { margin: 0; line-height: 1.5; font-size: 15px; }
  .wave {
    width: 64px;
    height: 64px;
    margin: 0 auto;
    border-radius: 50%;
    border: 2px solid #343b47;
    position: relative;
  }
  .wave::after {
    content: "";
    position: absolute;
    inset: -2px;
    border-radius: 50%;
    border: 2px solid var(--accent);
    animation: ring 2.2s ease-out infinite;
  }
  @keyframes ring { 0% { transform: scale(.7); opacity: .9; } 100% { transform: scale(1.6); opacity: 0; } }
  footer {
    display: flex;
    gap: 10px;
    align-items: center;
    flex-wrap: wrap;
    padding: 10px 16px;
    padding-bottom: calc(10px + env(safe-area-inset-bottom));
    background: var(--panel);
    border-top: 1px solid var(--line);
  }
  footer .meta {
    color: var(--muted);
    font-size: 12px;
    margin-right: auto;
    min-width: 0;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  #volume {
    width: 110px;
    accent-color: var(--accent);
    cursor: pointer;
  }
  #volume[hidden] { display: none; }
  #sound.live {
    border-color: var(--accent);
    color: var(--accent);
  }
  #sound.pending { animation: pulse 2s infinite; }
  @media (max-width: 640px) {
    header { padding: 8px 12px; gap: 8px; }
    .brand { font-size: 15px; }
    #status { order: 3; width: 100%; margin-left: 0; }
    select { max-width: none; flex: 1; }
    footer { gap: 8px; }
    footer .meta { flex-basis: 100%; white-space: normal; }
    footer button, footer a.button { flex: 1 1 28%; }
    #volume { flex: 1 1 100%; width: auto; }
  }
</style>
</head>
<body>
<header>
  <div class="brand">Vibe<strong>Z</strong> · Tela ao vivo</div>
  <select id="picker" aria-label="Escolha quem compartilha a tela">
    <option value="0">Automático (mais recente)</option>
  </select>
  <div id="status"><span id="dot" class="dot"></span><span id="statusText">conectando…</span></div>
</header>
<main id="stage">
  <img id="screen" alt="Tela compartilhada">
  <div id="placeholder">
    <div class="wave"></div>
    <h1 id="placeholderTitle">Aguardando alguém compartilhar a tela</h1>
    <p id="placeholderText">Abra o <strong>VibeZ</strong>, entre em um canal e clique em <strong>Compartilhar tela</strong>.</p>
  </div>
</main>
<footer>
  <div class="meta" id="meta">—</div>
  <button id="sound" type="button">Ativar som</button>
  <input id="volume" type="range" min="0" max="100" step="1" value="80" aria-label="Volume do som compartilhado" hidden>
  <button id="fullscreen" type="button">Tela cheia</button>
  <a class="button" id="download" href="#" download="vibez-tela.jpg">Baixar imagem</a>
  <button id="reload" type="button">Recarregar</button>
</footer>
<script>
(() => {
  'use strict';

  const stage = document.getElementById('stage');
  const screenEl = document.getElementById('screen');
  const placeholder = document.getElementById('placeholder');
  const placeholderTitle = document.getElementById('placeholderTitle');
  const placeholderText = document.getElementById('placeholderText');
  const picker = document.getElementById('picker');
  const dot = document.getElementById('dot');
  const statusText = document.getElementById('statusText');
  const meta = document.getElementById('meta');
  const download = document.getElementById('download');
  const fullscreenButton = document.getElementById('fullscreen');
  const reloadButton = document.getElementById('reload');
  const soundButton = document.getElementById('sound');
  const volume = document.getElementById('volume');

  const state = {
    streams: [],
    selection: 0,      // 0 follows the newest sharer automatically.
    requested: -1,     // Session id currently requested from the server.
    mode: 'mjpeg',
    pollTimer: null,
    objectUrl: null,
    pickerSignature: '',
  };

  // Shared-computer sound. Browsers only allow audio after a user gesture, so
  // the stream starts on the "Ativar som" button (or the first tap when the
  // preference was saved from a previous visit).
  const audio = {
    ctx: null,
    gain: null,
    on: false,
    running: false,
    runId: 0,
    controller: null,
    nextTime: 0,
    carry: null,
    sources: [],
    selection: null,
    lastBytesAt: 0,
  };

  try {
    audio.on = window.localStorage.getItem('vibez-sound') === '1';
    const savedVolume = Number(window.localStorage.getItem('vibez-volume'));
    if (Number.isFinite(savedVolume) && savedVolume >= 0 && savedVolume <= 100) volume.value = String(savedVolume);
  } catch (error) {
    // Storage can be disabled; sound still works for this visit.
  }

  function updateSoundButton() {
    soundButton.textContent = audio.running ? 'Som ligado' : 'Ativar som';
    soundButton.classList.toggle('live', audio.running);
    soundButton.classList.toggle('pending', audio.on && !audio.running);
    volume.hidden = !audio.on;
  }

  async function ensureAudioContext() {
    const Ctx = window.AudioContext || window.webkitAudioContext;
    if (!Ctx) return false;
    if (!audio.ctx) {
      audio.ctx = new Ctx({ latencyHint: 'interactive' });
      audio.gain = audio.ctx.createGain();
      audio.gain.gain.value = Number(volume.value) / 100;
      audio.gain.connect(audio.ctx.destination);
    }
    try {
      if (audio.ctx.state === 'suspended') await audio.ctx.resume();
    } catch (error) {
      return false;
    }
    return audio.ctx.state === 'running';
  }

  function stopAudio() {
    audio.running = false;
    ++audio.runId;
    if (audio.controller) {
      audio.controller.abort();
      audio.controller = null;
    }
    audio.carry = null;
    audio.nextTime = 0;
    for (const source of audio.sources) {
      try { source.stop(); } catch (error) { /* Already ended. */ }
    }
    audio.sources = [];
    updateSoundButton();
  }

  function schedulePcm(samples) {
    const ctx = audio.ctx;
    if (!ctx || !samples.length) return;
    if (!audio.nextTime || audio.nextTime < ctx.currentTime - 0.05) {
      // A small prebuffer absorbs network jitter without adding audible delay.
      audio.nextTime = ctx.currentTime + 0.18;
    }
    // If we ever run far ahead (clock drift), drop fresh data instead of
    // stacking minutes of latency.
    if (audio.nextTime - ctx.currentTime > 0.8) return;

    const buffer = ctx.createBuffer(1, samples.length, 48000);
    buffer.getChannelData(0).set(samples);
    const source = ctx.createBufferSource();
    source.buffer = buffer;
    source.connect(audio.gain);
    source.start(audio.nextTime);
    audio.nextTime += buffer.duration;
    audio.sources.push(source);
    source.onended = () => {
      const index = audio.sources.indexOf(source);
      if (index >= 0) audio.sources.splice(index, 1);
    };
  }

  function feedPcm(chunk) {
    let data = chunk;
    if (audio.carry && audio.carry.length) {
      const merged = new Uint8Array(audio.carry.length + data.length);
      merged.set(audio.carry);
      merged.set(data, audio.carry.length);
      data = merged;
    }
    const count = Math.floor(data.length / 2);
    if (count === 0) {
      audio.carry = data;
      return;
    }
    const view = new DataView(data.buffer, data.byteOffset, count * 2);
    const samples = new Float32Array(count);
    for (let index = 0; index < count; ++index) {
      samples[index] = view.getInt16(index * 2, true) / 32768;
    }
    audio.carry = data.slice(count * 2);
    schedulePcm(samples);
  }

  async function startAudio() {
    if (!audio.on) return;
    audio.selection = state.selection;
    if (!(await ensureAudioContext())) {
      audio.on = false;
      audio.selection = null;
      updateSoundButton();
      return;
    }
    stopAudio();

    const runId = ++audio.runId;
    audio.running = true;
    audio.carry = null;
    audio.nextTime = 0;
    const controller = new AbortController();
    audio.controller = controller;
    updateSoundButton();

    const requestId = state.selection;
    try {
      const response = await fetch('/audio.pcm?session=' + requestId + '&t=' + Date.now(),
                                   { cache: 'no-store', signal: controller.signal });
      if (!response.ok || !response.body) throw new Error('HTTP ' + response.status);
      const reader = response.body.getReader();
      while (audio.running && audio.runId === runId) {
        const { value, done } = await reader.read();
        if (done) break;
        if (value && value.length) {
          audio.lastBytesAt = performance.now();
          feedPcm(value);
        }
      }
    } catch (error) {
      // The retry below runs while the user still wants sound.
    }

    if (audio.running && audio.runId === runId) {
      // The server closes the stream when the share ends or the listener is
      // slow. Reconnect shortly, and stop cleanly if nobody is sharing.
      window.setTimeout(() => {
        if (audio.running && audio.runId === runId) {
          if (state.streams.length) startAudio();
          else { stopAudio(); audio.selection = null; }
        }
      }, 1200);
    }
  }

  soundButton.addEventListener('click', async () => {
    if (!audio.on) {
      audio.on = true;
      try { window.localStorage.setItem('vibez-sound', '1'); } catch (error) { /* ignore */ }
      updateSoundButton();
      await startAudio();
    } else if (!audio.running) {
      await startAudio();
    } else {
      audio.on = false;
      try { window.localStorage.setItem('vibez-sound', '0'); } catch (error) { /* ignore */ }
      stopAudio();
    }
  });

  volume.addEventListener('input', () => {
    if (audio.gain) audio.gain.gain.value = Number(volume.value) / 100;
    try { window.localStorage.setItem('vibez-volume', volume.value); } catch (error) { /* ignore */ }
  });

  updateSoundButton();

  function setStatus(label, live) {
    statusText.textContent = label;
    dot.classList.toggle('live', !!live);
  }

  function showWaiting() {
    placeholderTitle.textContent = 'Aguardando alguém compartilhar a tela';
    placeholderText.innerHTML = 'Abra o <strong>VibeZ</strong>, entre em um canal e clique em <strong>Compartilhar tela</strong>.';
    placeholder.hidden = false;
    screenEl.classList.remove('visible');
  }

  function effectiveStream() {
    if (state.selection === 0) return state.streams.length ? state.streams[0] : null;
    return state.streams.find((stream) => stream.id === state.selection) || null;
  }

  function stopStream() {
    if (state.pollTimer) {
      clearTimeout(state.pollTimer);
      state.pollTimer = null;
    }
    screenEl.onerror = null;
    screenEl.removeAttribute('src');
    if (state.objectUrl) {
      URL.revokeObjectURL(state.objectUrl);
      state.objectUrl = null;
    }
    state.requested = -1;
    state.mode = 'mjpeg';
  }

  function startStream(requestId) {
    stopStream();
    state.requested = requestId;
    state.mode = 'mjpeg';
    placeholder.hidden = false;
    screenEl.classList.remove('visible');
    screenEl.onerror = () => {
      // Some browsers refuse multipart/x-mixed-replace; fall back to snapshots.
      if (state.requested === requestId && state.mode === 'mjpeg') startPolling(requestId);
    };
    screenEl.src = '/stream.mjpeg?session=' + requestId + '&t=' + Date.now();
  }

  function startPolling(requestId) {
    stopStream();
    state.requested = requestId;
    state.mode = 'poll';
    const tick = async () => {
      if (state.mode !== 'poll' || state.requested !== requestId) return;
      try {
        const response = await fetch('/snapshot.jpg?session=' + requestId + '&t=' + Date.now(), { cache: 'no-store' });
        if (response.ok) {
          const blob = await response.blob();
          const url = URL.createObjectURL(blob);
          const previous = state.objectUrl;
          state.objectUrl = url;
          screenEl.src = url;
          if (previous) setTimeout(() => URL.revokeObjectURL(previous), 2000);
        }
      } catch (error) {
        // The timer below keeps retrying while the server is unreachable.
      }
      if (state.mode === 'poll' && state.requested === requestId) {
        state.pollTimer = setTimeout(tick, 200);
      }
    };
    state.pollTimer = setTimeout(tick, 0);
  }

  function updatePicker() {
    const signature = state.streams.map((stream) => stream.id + ':' + stream.name + ':' + stream.channel).join('|');
    if (signature !== state.pickerSignature) {
      state.pickerSignature = signature;
      const options = [new Option('Automático (mais recente)', '0')];
      for (const stream of state.streams) {
        options.push(new Option(stream.name + ' · ' + stream.channel, String(stream.id)));
      }
      picker.replaceChildren(...options);
    }
    picker.value = String(state.selection);
  }

  async function refreshStreams() {
    try {
      const response = await fetch('/api/streams', { cache: 'no-store' });
      if (!response.ok) throw new Error('HTTP ' + response.status);
      const data = await response.json();
      state.streams = Array.isArray(data.streams) ? data.streams : [];
      if (state.selection !== 0 && !state.streams.some((stream) => stream.id === state.selection)) {
        state.selection = 0; // The selected participant stopped sharing.
      }
      updatePicker();
      const current = effectiveStream();
      if (!current) {
        stopAudio();
        audio.selection = null;
        setStatus('aguardando transmissão', false);
        const listeners = data.listeners ? ' · ' + data.listeners + ' ouvindo' : '';
        meta.textContent = (data.viewers ? data.viewers + ' espectador(es) conectado(s)' : 'nenhum compartilhamento ativo') + listeners;
        if (state.requested !== -1) stopStream();
        showWaiting();
        return;
      }
      if (state.requested !== state.selection) startStream(state.selection);
      if (audio.on && audio.selection !== state.selection) startAudio();
      const live = current.fps > 0.3;
      setStatus(live ? 'ao vivo' : 'conectando…', live);
      const viewers = data.viewers ? ' · ' + data.viewers + ' espectador(es)' : '';
      const listeners = data.listeners ? ' · ' + data.listeners + ' ouvindo' : '';
      const sound = current.audio ? ' · som' : ' · sem som';
      meta.textContent = current.name + ' · ' + current.channel + ' · ' + current.width + '×' + current.height +
        ' · ' + current.fps.toFixed(1) + ' fps' + sound + viewers + listeners;
      download.href = '/snapshot.jpg?session=' + state.selection + '&t=' + Date.now();
    } catch (error) {
      // Never swallow this silently: a malformed payload (invalid JSON) looks
      // exactly like an offline server on screen, and an empty console makes it
      // impossible to tell them apart.
      console.error('VibeZ: falha ao atualizar as transmissões', error);
      setStatus('servidor fora do ar', false);
    }
  }

  picker.addEventListener('change', () => {
    state.selection = Number(picker.value) || 0;
    startStream(state.selection);
    if (audio.on) startAudio();
    refreshStreams();
  });

  fullscreenButton.addEventListener('click', async () => {
    try {
      if (document.fullscreenElement) {
        await document.exitFullscreen();
      } else if (stage.requestFullscreen) {
        await stage.requestFullscreen();
      } else if (stage.webkitRequestFullscreen) {
        stage.webkitRequestFullscreen();
      }
    } catch (error) {
      // Fullscreen is a convenience; ignore browsers that refuse it.
    }
  });
  if (!document.fullscreenEnabled && !stage.webkitRequestFullscreen) fullscreenButton.hidden = true;

  reloadButton.addEventListener('click', () => {
    state.requested = -1;
    refreshStreams();
    startStream(state.selection);
    if (audio.on) startAudio();
  });

  document.addEventListener('visibilitychange', () => {
    if (document.hidden) return;
    // Mobile browsers pause long-lived image streams in the background.
    state.requested = -1;
    refreshStreams();
    startStream(state.selection);
    if (audio.on) startAudio();
  });

  // A decoded frame is enough to hide the waiting overlay, both for MJPEG and
  // for the snapshot fallback.
  setInterval(() => {
    if (screenEl.naturalWidth > 0) {
      screenEl.classList.add('visible');
      placeholder.hidden = true;
    }
  }, 300);

  refreshStreams();
  setInterval(refreshStreams, 2000);
})();
</script>
</body>
</html>
)HTML";

} // namespace vibez::server
