// network/WebRemote.hpp - Interface Web de Controle Remoto e Transmissão do SwitchCast
#pragma once

#include <string>

namespace SwitchCast {

inline const char* GetWebRemoteHtml() {
    return R"HTML_CONTENT(<!DOCTYPE html>
<html lang="pt-BR">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>SwitchCast - Controle Remoto &amp; Transmiss&atilde;o</title>
  <link rel="icon" type="image/jpeg" href="/icon.jpg">
  <style>
    :root {
      --bg: #090b10;
      --card-bg: rgba(18, 24, 38, 0.78);
      --card-border: rgba(255, 255, 255, 0.08);
      --cyan: #00E5FF;
      --cyan-glow: rgba(0, 229, 255, 0.35);
      --red: #FF2A55;
      --red-glow: rgba(255, 42, 85, 0.35);
      --green: #10B981;
      --amber: #F59E0B;
      --text: #F3F4F6;
      --text-muted: #9CA3AF;
      --surface: rgba(10, 14, 22, 0.85);
      --radius: 16px;
    }
    * { box-sizing: border-box; margin: 0; padding: 0; }
    body {
      background:
        radial-gradient(circle at 10% 10%, rgba(0, 229, 255, 0.09) 0%, transparent 40%),
        radial-gradient(circle at 90% 10%, rgba(255, 42, 85, 0.09) 0%, transparent 40%),
        radial-gradient(circle at top center, #111a2e 0%, var(--bg) 60%);
      color: var(--text);
      font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, Helvetica, Arial, sans-serif;
      min-height: 100vh;
      padding: 20px 16px 40px;
      display: flex;
      flex-direction: column;
      align-items: center;
      overflow-x: hidden;
    }
    /* Animated Video Wallpaper */
    .bg-video {
      position: fixed;
      top: 0;
      left: 0;
      width: 100vw;
      height: 100vh;
      object-fit: cover;
      z-index: -2;
      opacity: 0.38;
      filter: saturate(1.25) brightness(0.85);
      pointer-events: none;
      transition: opacity 1s ease;
    }
    .bg-overlay {
      position: fixed;
      top: 0;
      left: 0;
      width: 100vw;
      height: 100vh;
      z-index: -1;
      background: radial-gradient(circle at 50% 20%, rgba(9, 11, 16, 0.40) 0%, rgba(9, 11, 16, 0.88) 100%);
      pointer-events: none;
    }
    .container {
      width: 100%;
      max-width: 760px;
      display: flex;
      flex-direction: column;
      gap: 20px;
    }
    header {
      display: flex;
      justify-content: space-between;
      align-items: center;
      padding: 12px 6px;
      flex-wrap: wrap;
      gap: 12px;
    }
    .logo-box {
      display: flex;
      align-items: center;
      gap: 12px;
    }
    .logo-icon {
      width: 44px;
      height: 44px;
      background: linear-gradient(135deg, #00A3FF, var(--cyan));
      border-radius: 12px;
      display: flex;
      align-items: center;
      justify-content: center;
      box-shadow: 0 0 20px var(--cyan-glow);
    }
    .logo-icon svg { width: 24px; height: 24px; fill: #000; }
    .brand-title {
      font-size: 24px;
      font-weight: 800;
      letter-spacing: -0.5px;
      background: linear-gradient(90deg, #FFFFFF, var(--cyan));
      -webkit-background-clip: text;
      -webkit-text-fill-color: transparent;
    }
    .brand-sub { font-size: 12px; color: var(--text-muted); letter-spacing: 1px; font-weight: 600; }
    .status-badge {
      display: inline-flex;
      align-items: center;
      gap: 8px;
      padding: 6px 14px;
      border-radius: 20px;
      background: rgba(16, 185, 129, 0.15);
      border: 1px solid rgba(16, 185, 129, 0.4);
      font-size: 13px;
      font-weight: 600;
      color: #34D399;
      transition: all 0.3s;
    }
    .dot {
      width: 8px;
      height: 8px;
      background-color: #10B981;
      border-radius: 50%;
      box-shadow: 0 0 8px #10B981;
      animation: pulse 2s infinite;
    }
    @keyframes pulse { 0%, 100% { opacity: 1; transform: scale(1); } 50% { opacity: 0.4; transform: scale(0.85); } }

    .card {
      background: var(--card-bg);
      backdrop-filter: blur(16px);
      -webkit-backdrop-filter: blur(16px);
      border: 1px solid var(--card-border);
      border-radius: var(--radius);
      padding: 24px;
      box-shadow: 0 8px 32px rgba(0, 0, 0, 0.35);
      transition: border-color 0.2s;
    }
    .card:hover { border-color: rgba(0, 229, 255, 0.25); }
    .card-title {
      font-size: 18px;
      font-weight: 700;
      margin-bottom: 16px;
      display: flex;
      align-items: center;
      gap: 10px;
      color: #E2E8F0;
    }
    .card-title svg { width: 20px; height: 20px; fill: var(--cyan); }

    /* Now Playing */
    .now-playing-box {
      background: var(--surface);
      border: 1px solid rgba(255, 255, 255, 0.05);
      border-radius: 12px;
      padding: 18px;
      margin-bottom: 20px;
    }
    .media-title {
      font-size: 20px;
      font-weight: 700;
      margin-bottom: 8px;
      white-space: nowrap;
      overflow: hidden;
      text-overflow: ellipsis;
      color: #FFF;
    }
    .media-state {
      display: inline-block;
      font-size: 12px;
      font-weight: 700;
      letter-spacing: 0.5px;
      padding: 4px 10px;
      border-radius: 8px;
      background: rgba(255, 255, 255, 0.08);
      color: var(--text-muted);
      margin-bottom: 16px;
    }
    .media-state.playing { background: rgba(16, 185, 129, 0.2); color: #34D399; border: 1px solid rgba(16, 185, 129, 0.3); }
    .media-state.paused { background: rgba(245, 158, 11, 0.2); color: #FBBF24; border: 1px solid rgba(245, 158, 11, 0.3); }

    /* Progress bar */
    .progress-wrap { margin-bottom: 16px; }
    .time-labels {
      display: flex;
      justify-content: space-between;
      font-size: 13px;
      font-variant-numeric: tabular-nums;
      color: var(--text-muted);
      margin-top: 6px;
    }
    input[type=range] {
      -webkit-appearance: none;
      width: 100%;
      height: 8px;
      border-radius: 4px;
      background: #1F293D;
      outline: none;
      cursor: pointer;
    }
    input[type=range]::-webkit-slider-thumb {
      -webkit-appearance: none;
      width: 18px;
      height: 18px;
      border-radius: 50%;
      background: var(--cyan);
      box-shadow: 0 0 10px var(--cyan);
      cursor: pointer;
      transition: transform 0.1s;
    }
    input[type=range]::-webkit-slider-thumb:hover { transform: scale(1.2); }

    /* Controls */
    .controls-grid {
      display: flex;
      justify-content: center;
      align-items: center;
      gap: 16px;
      margin-bottom: 20px;
    }
    .btn {
      background: #1E293B;
      color: var(--text);
      border: 1px solid rgba(255, 255, 255, 0.1);
      border-radius: 12px;
      padding: 12px 18px;
      font-size: 15px;
      font-weight: 600;
      cursor: pointer;
      display: inline-flex;
      align-items: center;
      justify-content: center;
      gap: 8px;
      transition: all 0.15s ease-in-out;
      user-select: none;
    }
    .btn:hover {
      background: #334155;
      border-color: rgba(255, 255, 255, 0.2);
      transform: translateY(-1px);
    }
    .btn:active { transform: translateY(1px); }
    .btn-primary {
      background: linear-gradient(135deg, #00A3FF, var(--cyan));
      color: #000;
      border: none;
      font-weight: 700;
      box-shadow: 0 4px 14px var(--cyan-glow);
    }
    .btn-primary:hover {
      box-shadow: 0 6px 20px var(--cyan-glow);
      background: linear-gradient(135deg, #38BDF8, #22D3EE);
    }
    .btn-circle {
      width: 58px;
      height: 58px;
      border-radius: 50%;
      padding: 0;
      font-size: 22px;
    }
    .btn-sm { padding: 8px 14px; font-size: 13px; border-radius: 8px; }

    /* Volume */
    .volume-row {
      display: flex;
      align-items: center;
      gap: 14px;
      padding-top: 10px;
      border-top: 1px solid rgba(255, 255, 255, 0.05);
    }
    .volume-icon { color: var(--cyan); font-size: 18px; }

    /* Cast Form */
    .form-group {
      display: flex;
      flex-direction: column;
      gap: 8px;
      margin-bottom: 14px;
    }
    label { font-size: 13px; font-weight: 600; color: #CBD5E1; }
    input[type=text] {
      background: var(--surface);
      border: 1px solid rgba(255, 255, 255, 0.12);
      border-radius: 10px;
      padding: 12px 14px;
      color: #FFF;
      font-size: 14px;
      outline: none;
      transition: border-color 0.2s, box-shadow 0.2s;
    }
    input[type=text]:focus {
      border-color: var(--cyan);
      box-shadow: 0 0 12px var(--cyan-glow);
    }
    .btn-block { width: 100%; padding: 14px; font-size: 16px; margin-top: 6px; }

    /* Preset Chips */
    .presets-title { font-size: 13px; color: var(--text-muted); margin: 16px 0 8px; font-weight: 600; }
    .chips-grid {
      display: flex;
      flex-wrap: wrap;
      gap: 8px;
    }
    .chip {
      background: rgba(255, 255, 255, 0.05);
      border: 1px solid rgba(255, 255, 255, 0.1);
      border-radius: 20px;
      padding: 8px 14px;
      font-size: 13px;
      color: #E2E8F0;
      cursor: pointer;
      transition: all 0.15s;
    }
    .chip:hover {
      background: rgba(0, 229, 255, 0.15);
      border-color: var(--cyan);
      color: #FFF;
      transform: translateY(-1px);
    }

    /* Help Section */
    .guide-box {
      font-size: 14px;
      color: #94A3B8;
      line-height: 1.6;
    }
    .guide-item {
      display: flex;
      gap: 10px;
      margin-bottom: 10px;
    }
    .guide-badge {
      background: #1E293B;
      color: var(--cyan);
      font-size: 11px;
      font-weight: 700;
      padding: 2px 8px;
      border-radius: 6px;
      height: fit-content;
      margin-top: 2px;
      white-space: nowrap;
    }

    /* Toast */
    #toast {
      position: fixed;
      bottom: 24px;
      left: 50%;
      transform: translateX(-50%) translateY(100px);
      background: #1E293B;
      color: #FFF;
      border: 1px solid var(--cyan);
      padding: 12px 24px;
      border-radius: 30px;
      font-size: 14px;
      font-weight: 600;
      box-shadow: 0 10px 25px rgba(0, 0, 0, 0.5), 0 0 15px var(--cyan-glow);
      opacity: 0;
      transition: transform 0.3s cubic-bezier(0.18, 0.89, 0.32, 1.28), opacity 0.3s;
      z-index: 999;
      pointer-events: none;
    }
    #toast.show {
      transform: translateX(-50%) translateY(0);
      opacity: 1;
    }
  </style>
</head>
<body>

  <!-- Video Wallpaper Animado (Red & Blue Waves) -->
  <video id="bgVideo" class="bg-video" autoplay loop muted playsinline poster="/icon.jpg">
    <source src="/wallpaper.mp4" type="video/mp4">
  </video>
  <div class="bg-overlay"></div>

  <div class="container">
    <header>
      <div class="logo-box">
        <img src="/icon.jpg" alt="SwitchCast" style="width:48px; height:48px; border-radius:12px; box-shadow: -4px 0 15px var(--cyan-glow), 4px 0 15px var(--red-glow); object-fit: cover;">
        <div>
          <div class="brand-title"><span style="color:var(--cyan);">SWITCH</span><span style="color:var(--red);">CAST</span></div>
          <div class="brand-sub">v1.4.0 &bull; POR ALEFF959</div>
        </div>
      </div>
      <div id="connectionStatus" class="status-badge">
        <div class="dot"></div>
        <span id="statusText">Conectado ao Switch</span>
      </div>
    </header>

    <!-- Now Playing -->
    <div class="card">
      <div class="card-title">
        <svg viewBox="0 0 24 24"><path d="M12 2C6.48 2 2 6.48 2 12s4.48 10 10 10 10-4.48 10-10S17.52 2 12 2zm-2 14.5v-9l6 4.5-6 4.5z"/></svg>
        Em Reprodu&ccedil;&atilde;o no Nintendo Switch
      </div>
      <div class="now-playing-box">
        <div id="mediaTitle" class="media-title">Nenhuma m&iacute;dia ativa</div>
        <span id="mediaState" class="media-state">OCIOSO</span>

        <div class="progress-wrap">
          <input type="range" id="seekSlider" min="0" max="100" value="0">
          <div class="time-labels">
            <span id="currentTime">00:00</span>
            <span id="totalDuration">00:00</span>
          </div>
        </div>

        <div class="controls-grid">
          <button class="btn btn-sm" onclick="sendCmd('seek', {offsetMs: -10000})">&#9194; -10s</button>
          <button id="btnPlayPause" class="btn btn-primary btn-circle" onclick="togglePlayPause()">&#9654;</button>
          <button class="btn btn-sm" onclick="sendCmd('stop')">&#9209; Parar</button>
          <button class="btn btn-sm" onclick="sendCmd('seek', {offsetMs: 10000})">+10s &#9193;</button>
        </div>

        <div class="volume-row">
          <span class="volume-icon">&#128266;</span>
          <input type="range" id="volSlider" min="0" max="100" value="80" oninput="onVolumeChange(this.value)">
          <span id="volLabel" style="font-size:13px; font-weight:700; min-width:44px;">80%</span>
          <button id="btnMute" class="btn btn-sm" onclick="toggleMute()">Mudo</button>
        </div>
      </div>

      <div style="display:flex; gap:10px; flex-wrap:wrap;">
        <button class="btn btn-sm" onclick="sendCmd('cycleAudio')">&#127911; Pr&oacute;xima Faixa de &Aacute;udio</button>
        <button class="btn btn-sm" onclick="sendCmd('cycleSubtitle')">&#128172; Pr&oacute;xima Legenda</button>
      </div>
    </div>

    <!-- Cast Media -->
    <div class="card">
      <div class="card-title">
        <svg viewBox="0 0 24 24"><path d="M19.35 10.04C18.67 6.59 15.64 4 12 4 9.11 4 6.6 5.64 5.35 8.04 2.34 8.36 0 10.91 0 14c0 3.31 2.69 6 6 6h13c2.76 0 5-2.24 5-5 0-2.64-2.05-4.78-4.65-4.96zM14 13v4h-4v-4H7l5-5 5 5h-3z"/></svg>
        Transmitir V&iacute;deo ou &Aacute;udio (Link Direto)
      </div>

      <div class="form-group">
        <label for="mediaUrl">URL do V&iacute;deo ou Stream (MP4, MKV, M3U8, HLS, TS, etc.):</label>
        <input type="text" id="mediaUrl" placeholder="https://exemplo.com/video.mp4">
      </div>
      <div class="form-group">
        <label for="mediaCustomTitle">T&iacute;tulo (Opcional):</label>
        <input type="text" id="mediaCustomTitle" placeholder="Ex: Meu Filme 1080p">
      </div>

      <button class="btn btn-primary btn-block" onclick="castMedia()">&#9654; TRANSMITIR PARA O SWITCH</button>

      <div class="presets-title">V&iacute;deos de Teste R&aacute;pido (1 Clique):</div>
      <div class="chips-grid">
        <div class="chip" onclick="fillAndPlay('https://commondatastream.googleapis.com/gtv-videos-bucket/sample/BigBuckBunny.mp4', 'Big Buck Bunny (1080p 60fps)')">&#127916; Big Buck Bunny 1080p</div>
        <div class="chip" onclick="fillAndPlay('https://commondatastream.googleapis.com/gtv-videos-bucket/sample/ElephantsDream.mp4', 'Elephants Dream (720p)')">&#127916; Elephants Dream 720p</div>
        <div class="chip" onclick="fillAndPlay('https://bitdash-a.akamaihd.net/content/sintel/hls/playlist.m3u8', 'Sintel (HLS Multi-Bitrate)')">&#127871; Sintel (HLS M3U8)</div>
      </div>
    </div>

    <!-- DLNA Guide -->
    <div class="card">
      <div class="card-title">
        <svg viewBox="0 0 24 24"><path d="M17 1.01L7 1c-1.1 0-2 .9-2 2v18c0 1.1.9 2 2 2h10c1.1 0 2-.9 2-2V3c0-1.1-.9-1.99-2-1.99zM17 19H7V5h10v14z"/></svg>
        Como Transmitir Direto pelo Celular (DLNA / UPnP)
      </div>
      <div class="guide-box">
        <div class="guide-item">
          <div class="guide-badge">ANDROID</div>
          <div>Instale o <b>BubbleUPnP</b> ou <b>Web Video Caster</b>. Na aba <i>Renderers / Dispositivos</i>, selecione <b>SwitchCast</b> e transmita qualquer v&iacute;deo da web ou mem&oacute;ria.</div>
        </div>
        <div class="guide-item">
          <div class="guide-badge">IPHONE / IPAD</div>
          <div>Instale o app gratuito <b>Web Video Cast</b> ou <b>TV Assist</b> na App Store. Conecte no <b>SwitchCast</b> e envie v&iacute;deos do navegador.</div>
        </div>
        <div class="guide-item">
          <div class="guide-badge">PC / MAC</div>
          <div>No <b>VLC Media Player</b>, v&aacute; no menu <i>Reprodu&ccedil;&atilde;o &gt; Renderer</i> e escolha <b>SwitchCast</b>.</div>
        </div>
      </div>
    </div>
  </div>

  <div id="toast">Notifica&ccedil;&atilde;o</div>

  <script>
    let ws = null;
    let isPlaying = false;
    let durationMs = 0;
    let isUserSeeking = false;
    let isMuted = false;

    function showToast(msg) {
      const t = document.getElementById('toast');
      t.innerText = msg;
      t.classList.add('show');
      setTimeout(() => t.classList.remove('show'), 2800);
    }

    function formatTime(ms) {
      if (!ms || ms < 0) return "00:00";
      const totalSec = Math.floor(ms / 1000);
      const h = Math.floor(totalSec / 3600);
      const m = Math.floor((totalSec % 3600) / 60);
      const s = totalSec % 60;
      if (h > 0) {
        return `${h}:${m.toString().padStart(2, '0')}:${s.toString().padStart(2, '0')}`;
      }
      return `${m.toString().padStart(2, '0')}:${s.toString().padStart(2, '0')}`;
    }

    function connectWs() {
      const host = window.location.hostname;
      const port = window.location.port || "8080";
      const wsUrl = `ws://${host}:${port}/ws`;
      
      try {
        ws = new WebSocket(wsUrl);

        ws.onopen = () => {
          document.getElementById('statusText').innerText = "Conectado ao Switch";
          document.getElementById('connectionStatus').style.borderColor = "rgba(16, 185, 129, 0.4)";
          document.getElementById('connectionStatus').style.color = "#34D399";
          sendCmd('status');
        };

        ws.onmessage = (e) => {
          try {
            const data = JSON.parse(e.data);
            if (data.event === "status" || data.state) {
              updateUI(data);
            }
          } catch(err) {}
        };

        ws.onclose = () => {
          document.getElementById('statusText').innerText = "Reconectando...";
          document.getElementById('connectionStatus').style.borderColor = "rgba(239, 68, 68, 0.4)";
          document.getElementById('connectionStatus').style.color = "#F87171";
          setTimeout(connectWs, 2000);
        };
      } catch (e) {
        setTimeout(connectWs, 3000);
      }
    }

    function updateUI(s) {
      const stateEl = document.getElementById('mediaState');
      const titleEl = document.getElementById('mediaTitle');
      const btnPlay = document.getElementById('btnPlayPause');

      titleEl.innerText = (s.title && s.title.length > 0) ? s.title : (s.state !== 'Idle' ? "Transmiss\u00e3o de M\u00eddia" : "Nenhuma m\u00eddia ativa");
      durationMs = s.durationMs || 0;

      if (s.state === 'Playing') {
        isPlaying = true;
        stateEl.innerText = "REPRODUZINDO";
        stateEl.className = "media-state playing";
        btnPlay.innerHTML = "&#10074;&#10074;";
      } else if (s.state === 'Paused') {
        isPlaying = false;
        stateEl.innerText = "PAUSADO";
        stateEl.className = "media-state paused";
        btnPlay.innerHTML = "&#9654;";
      } else {
        isPlaying = false;
        stateEl.innerText = "OCIOSO";
        stateEl.className = "media-state";
        btnPlay.innerHTML = "&#9654;";
      }

      if (!isUserSeeking && durationMs > 0) {
        const slider = document.getElementById('seekSlider');
        slider.value = ((s.positionMs || 0) / durationMs) * 100;
        document.getElementById('currentTime').innerText = formatTime(s.positionMs || 0);
        document.getElementById('totalDuration').innerText = formatTime(durationMs);
      } else if (durationMs === 0) {
        document.getElementById('currentTime').innerText = "00:00";
        document.getElementById('totalDuration').innerText = "00:00";
        document.getElementById('seekSlider').value = 0;
      }

      if (s.volume !== undefined) {
        document.getElementById('volSlider').value = s.volume;
        document.getElementById('volLabel').innerText = s.volume + "%";
      }
      if (s.muted !== undefined) {
        isMuted = s.muted;
        document.getElementById('btnMute').innerText = isMuted ? "Mudo (Ativo)" : "Mudo";
        document.getElementById('btnMute').style.borderColor = isMuted ? "var(--red)" : "";
      }
    }

    function sendCmd(cmdName, payload = {}) {
      const body = { cmd: cmdName, ...payload };
      if (ws && ws.readyState === WebSocket.OPEN) {
        ws.send(JSON.stringify(body));
      } else {
        fetch('/api/v1/' + cmdName, {
          method: 'POST',
          headers: {'Content-Type': 'application/json'},
          body: JSON.stringify(body)
        }).catch(() => {});
      }
    }

    function togglePlayPause() {
      if (isPlaying) {
        sendCmd('pause');
        showToast("Pausado");
      } else {
        sendCmd('play');
        showToast("Reproduzindo");
      }
    }

    function toggleMute() {
      isMuted = !isMuted;
      sendCmd('mute', {mute: isMuted});
      showToast(isMuted ? "Som desativado" : "Som ativado");
    }

    function onVolumeChange(val) {
      document.getElementById('volLabel').innerText = val + "%";
      sendCmd('volume', {level: parseInt(val)});
    }

    const seekSlider = document.getElementById('seekSlider');
    seekSlider.addEventListener('mousedown', () => { isUserSeeking = true; });
    seekSlider.addEventListener('touchstart', () => { isUserSeeking = true; });
    seekSlider.addEventListener('change', (e) => {
      isUserSeeking = false;
      if (durationMs > 0) {
        const targetMs = Math.floor((e.target.value / 100) * durationMs);
        sendCmd('seek', {positionMs: targetMs});
      }
    });

    function castMedia() {
      const url = document.getElementById('mediaUrl').value.trim();
      const title = document.getElementById('mediaCustomTitle').value.trim();
      if (!url) {
        showToast("Por favor, digite ou cole a URL da m\u00eddia.");
        return;
      }
      sendCmd('play', {url: url, title: title || "Transmiss\u00e3o Web"});
      showToast("Enviando m\u00eddia para o Switch...");
    }

    function fillAndPlay(url, title) {
      document.getElementById('mediaUrl').value = url;
      document.getElementById('mediaCustomTitle').value = title;
      castMedia();
    }

    // Polling de fallback caso WS falhe
    setInterval(() => {
      if (!ws || ws.readyState !== WebSocket.OPEN) {
        fetch('/api/v1/status')
          .then(r => r.json())
          .then(data => updateUI(data))
          .catch(() => {});
      }
    }, 1500);

    // Autoplay resiliente para mobile e desktop
    const bgVid = document.getElementById('bgVideo');
    if (bgVid) {
      bgVid.play().catch(() => {
        const resumePlay = () => {
          bgVid.play();
          window.removeEventListener('click', resumePlay);
          window.removeEventListener('touchstart', resumePlay);
        };
        window.addEventListener('click', resumePlay, { once: true });
        window.addEventListener('touchstart', resumePlay, { once: true });
      });
    }

    connectWs();
  </script>
</body>
</html>
)HTML_CONTENT";
}

}  // namespace SwitchCast
