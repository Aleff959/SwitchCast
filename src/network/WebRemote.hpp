// network/WebRemote.hpp - Interface Web de Controle Remoto e Transmissão do SwitchCast
// Padrão Vitral Stealth: Botão Morph (Hambúrguer -> Switch), Tela 2 Frontal "Vamos dar o play?",
// Símbolos DVD Drifting e Gaveta Lateral com Abas em Linha Única.
#pragma once

#include <string>
#include "../common/Types.hpp"

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
      --bg-base: color-mix(in srgb, #000000 90%, #707075 10%);
      --switch-blue: #00C3E3;
      --switch-red: #FF3C28;
      --border-dark-solid: #383D48;
      --border-dark-deep: #242730;
      --text-white: #F4F4F5;
      --text-muted: #A1A1AA;
      --shadow-black: #000000;
      --win11-font: "Segoe UI Variable Display", "Segoe UI Variable Text", "Segoe UI", "Inter", -apple-system, BlinkMacSystemFont, sans-serif;
    }

    * { box-sizing: border-box; margin: 0; padding: 0; }

    body {
      background-color: #0D0E12;
      color: var(--text-white);
      font-family: var(--win11-font);
      font-feature-settings: "cv05", "cv08", "ss01";
      -webkit-font-smoothing: antialiased;
      min-height: 100vh;
      overflow-x: hidden;
      position: relative;
    }

    /* Video Wallpaper Animado (em segundo plano) */
    .bg-video {
      position: fixed;
      top: 0;
      left: 0;
      width: 100vw;
      height: 100vh;
      object-fit: cover;
      z-index: -3;
      opacity: 0.22;
      filter: saturate(1.2) brightness(0.7);
      pointer-events: none;
    }

    /* Tela 1 (Base 90% Preta / 10% Cinza) */
    .base-screen {
      position: relative;
      min-height: 100vh;
      width: 100%;
      background-color: var(--bg-base);
      display: flex;
      flex-direction: column;
      align-items: center;
      justify-content: center;
      padding: 32px 16px;
      overflow: hidden;
      box-shadow: 0 28px 70px var(--shadow-black);
    }

    /* Alvo de Desfoque ao Abrir Menu */
    .bg-scene-blur-target {
      transition: filter 0.36s cubic-bezier(0.16, 1, 0.3, 1), transform 0.36s cubic-bezier(0.16, 1, 0.3, 1);
      width: 100%;
      display: flex;
      flex-direction: column;
      align-items: center;
      justify-content: center;
    }
    .bg-scene-blur-target.is-blurred {
      filter: blur(2.5px) brightness(0.86);
      transform: scale(0.99);
    }

    /* Micro-textura PlayStation de Fundo */
    .ps-texture {
      position: absolute;
      top: 0;
      left: 0;
      width: 100%;
      height: 100%;
      opacity: 0.28;
      pointer-events: none;
      z-index: 0;
    }

    /* Badge Superior Direito da Tela 1 ("v2.3.0 | Comp. 27") */
    .top-right-badge {
      position: absolute;
      top: 18px;
      right: 20px;
      display: flex;
      align-items: center;
      gap: 8px;
      padding: 6px 14px;
      border-radius: 999px;
      background: rgba(0, 0, 0, 0.85);
      border: 1px solid var(--border-dark-deep);
      font-size: 13px;
      font-weight: 700;
      color: #E2E2E6;
      box-shadow: 0 4px 12px var(--shadow-black);
      z-index: 40;
      white-space: nowrap;
      text-shadow: 0 1px 2px var(--shadow-black);
    }
    .top-right-badge .dot {
      width: 7px;
      height: 7px;
      border-radius: 50%;
      background: var(--switch-blue);
      box-shadow: 0 0 8px var(--switch-blue);
    }

    /* Botão Morph (Canto Superior Esquerdo da Tela 1) */
    .morph-btn {
      position: fixed;
      top: 16px;
      left: 16px;
      width: 56px;
      height: 48px;
      border-radius: 14px;
      background: rgba(0, 0, 0, 0.90);
      border: 1.5px solid var(--border-dark-solid);
      display: flex;
      align-items: center;
      justify-content: center;
      cursor: pointer;
      z-index: 100;
      box-shadow: 0 4px 12px -2px #000000, 0 2px 6px rgba(0, 0, 0, 0.9);
      transition: border-color 0.3s ease, background-color 0.3s ease, transform 0.15s ease;
      outline: none;
    }
    .morph-btn:hover {
      border-color: var(--switch-blue);
      transform: scale(1.04);
    }
    .morph-btn:active {
      transform: scale(0.96);
    }

    .morph-stage {
      position: relative;
      width: 26px;
      height: 20px;
    }

    /* As 3 barras horizontais do hambúrguer */
    .hbar {
      position: absolute;
      left: 50%;
      top: 50%;
      width: 22px;
      height: 2.8px;
      background-color: #D4D4D8;
      border-radius: 2px;
      box-shadow: 0 1px 2px #000000;
      transition:
        transform 0.68s cubic-bezier(0.34, 1.45, 0.64, 1),
        width 0.62s cubic-bezier(0.34, 1.45, 0.64, 1),
        height 0.62s cubic-bezier(0.34, 1.45, 0.64, 1),
        background-color 0.55s ease,
        border-radius 0.55s ease,
        border 0.55s ease;
    }

    /* Pontos dos analógicos do Switch */
    .hbar::after {
      content: "";
      position: absolute;
      width: 3.2px;
      height: 3.2px;
      border-radius: 50%;
      background-color: #090B0E;
      opacity: 0;
      transform: scale(0.2);
      transition: opacity 0.25s ease 0.1s, transform 0.3s cubic-bezier(0.34, 1.56, 0.64, 1) 0.1s;
    }

    .hbar-1 { transform: translate(-50%, calc(-50% - 7px)); }
    .hbar-1::after { top: 3.5px; left: 50%; margin-left: -1.6px; }

    .hbar-2 { transform: translate(-50%, -50%); }

    .hbar-3 { transform: translate(-50%, calc(-50% + 7px)); }
    .hbar-3::after { bottom: 3.5px; left: 50%; margin-left: -1.6px; }

    /* Estado Transformado no Nintendo Switch (.is-switch) */
    .morph-btn.is-switch .hbar-1 {
      width: 7px;
      height: 22px;
      background-color: var(--switch-blue);
      border-radius: 4.5px 1.5px 1.5px 4.5px;
      transform: translate(calc(-50% - 10.5px), -50%);
    }
    .morph-btn.is-switch .hbar-1::after { opacity: 1; transform: scale(1); }

    .morph-btn.is-switch .hbar-2 {
      width: 12px;
      height: 22px;
      background-color: #0B0D11;
      border: 1.2px solid var(--border-dark-solid);
      border-radius: 2px;
      transform: translate(-50%, -50%);
    }

    .morph-btn.is-switch .hbar-3 {
      width: 7px;
      height: 22px;
      background-color: var(--switch-red);
      border-radius: 1.5px 4.5px 4.5px 1.5px;
      transform: translate(calc(-50% + 10.5px), -50%);
    }
    .morph-btn.is-switch .hbar-3::after { opacity: 1; transform: scale(1); }

    /* Tela 2 Frontal (Centralizada e Minimalista: "Vamos dar o play?") */
    .front-panel {
      position: relative;
      z-index: 10;
      width: 90%;
      max-width: 680px;
      background: rgba(14, 16, 21, 0.88);
      backdrop-filter: blur(38px) saturate(180%);
      -webkit-backdrop-filter: blur(38px) saturate(180%);
      border: 1.5px solid var(--border-dark-solid);
      border-radius: 24px;
      box-shadow:
        0 34px 68px -10px var(--shadow-black),
        0 16px 30px -6px rgba(0, 0, 0, 0.98),
        0 0 0 1px #1A1D24 inset;
      padding: 36px 28px;
      display: flex;
      flex-direction: column;
      align-items: center;
      gap: 22px;
      text-align: center;
    }

    .panel-brand-title {
      font-size: 26px;
      font-weight: 700;
      color: #F4F4F5;
      letter-spacing: -0.5px;
      text-shadow: 0 2px 4px var(--shadow-black), 0 4px 8px var(--shadow-black);
    }

    /* Box do Ícone Play (64x64px) centralizado */
    .play-icon-box {
      width: 64px;
      height: 64px;
      border-radius: 18px;
      background: rgba(0, 0, 0, 0.92);
      border: 1.5px solid var(--border-dark-solid);
      box-shadow: 0 10px 24px var(--shadow-black), 0 2px 6px rgba(0, 0, 0, 0.9);
      display: flex;
      align-items: center;
      justify-content: center;
      color: var(--switch-blue);
      font-size: 24px;
      cursor: pointer;
      transition: transform 0.2s, border-color 0.2s, box-shadow 0.2s;
    }
    .play-icon-box:hover {
      transform: scale(1.06);
      border-color: var(--switch-blue);
      box-shadow: 0 12px 30px var(--shadow-black), 0 0 16px rgba(0, 195, 227, 0.3);
    }

    .hero-title {
      font-size: 22px;
      font-weight: 700;
      color: #F4F4F5;
      text-shadow: 0 2px 4px var(--shadow-black);
    }

    .hero-subtitle {
      font-size: 15px;
      color: var(--text-muted);
      max-width: 440px;
      line-height: 1.45;
      text-shadow: 0 1px 3px var(--shadow-black);
    }

    /* Input & Cast Bar */
    .cast-bar {
      display: flex;
      gap: 8px;
      width: 100%;
      max-width: 520px;
    }
    .cast-input {
      flex: 1;
      height: 42px;
      background: #090B10;
      border: 1px solid var(--border-dark-solid);
      border-radius: 12px;
      padding: 0 14px;
      color: #FFFFFF;
      font-size: 13.5px;
      outline: none;
      box-shadow: inset 0 2px 4px var(--shadow-black);
      transition: border-color 0.2s, box-shadow 0.2s;
    }
    .cast-input:focus {
      border-color: var(--switch-blue);
      box-shadow: 0 0 10px rgba(0, 195, 227, 0.3), inset 0 2px 4px var(--shadow-black);
    }
    .cast-btn {
      height: 42px;
      padding: 0 18px;
      border-radius: 12px;
      font-size: 13px;
      font-weight: 700;
      color: #FFFFFF;
      background: linear-gradient(135deg, #0A637A 0%, #7A1D16 100%);
      border: 1px solid var(--border-dark-solid);
      box-shadow: 0 6px 16px var(--shadow-black);
      cursor: pointer;
      display: flex;
      align-items: center;
      gap: 6px;
      white-space: nowrap;
      text-shadow: 0 1px 2px var(--shadow-black);
      transition: transform 0.15s, filter 0.15s;
    }
    .cast-btn:hover { filter: brightness(1.15); transform: translateY(-1px); }
    .cast-btn:active { transform: scale(0.97); }

    /* Card de Reprodução Ativa */
    .playback-widget {
      width: 100%;
      max-width: 520px;
      background: rgba(0, 0, 0, 0.7);
      border: 1px solid var(--border-dark-solid);
      border-radius: 16px;
      padding: 16px 20px;
      display: flex;
      flex-direction: column;
      gap: 12px;
      box-shadow: 0 12px 28px var(--shadow-black);
    }
    .playback-info-row {
      display: flex;
      justify-content: space-between;
      align-items: center;
      gap: 10px;
      white-space: nowrap;
    }
    .playback-title {
      font-size: 14.5px;
      font-weight: 600;
      color: #FFFFFF;
      overflow: hidden;
      text-overflow: ellipsis;
      text-shadow: 0 1px 3px var(--shadow-black);
    }
    .playback-state-pill {
      font-size: 10.5px;
      font-weight: 700;
      padding: 3px 8px;
      border-radius: 6px;
      background: #181B22;
      border: 1px solid var(--border-dark-solid);
      color: var(--text-muted);
      flex-shrink: 0;
    }
    .playback-state-pill.playing {
      color: #34D399;
      border-color: rgba(52, 211, 153, 0.4);
      background: rgba(52, 211, 153, 0.12);
    }

    /* Slider de Seek */
    .seek-slider {
      -webkit-appearance: none;
      width: 100%;
      height: 6px;
      background: #1A1D24;
      border-radius: 999px;
      outline: none;
      box-shadow: inset 0 1px 3px var(--shadow-black);
    }
    .seek-slider::-webkit-slider-thumb {
      -webkit-appearance: none;
      width: 14px;
      height: 14px;
      border-radius: 50%;
      background: var(--switch-blue);
      box-shadow: 0 0 10px var(--switch-blue);
      cursor: pointer;
      border: 1px solid #FFFFFF;
    }
    .time-labels {
      display: flex;
      justify-content: space-between;
      font-size: 11px;
      color: var(--text-muted);
      text-shadow: 0 1px 2px var(--shadow-black);
    }

    /* Botões de Controle em 1 Linha Estrita */
    .controls-row {
      display: flex;
      align-items: center;
      justify-content: space-between;
      gap: 8px;
      width: 100%;
      flex-wrap: nowrap;
      white-space: nowrap;
    }
    .btn-ctrl {
      height: 34px;
      padding: 0 12px;
      border-radius: 10px;
      font-size: 12px;
      font-weight: 700;
      color: #E2E2E6;
      background: rgba(0, 0, 0, 0.85);
      border: 1px solid var(--border-dark-solid);
      box-shadow: 0 4px 12px var(--shadow-black);
      cursor: pointer;
      display: flex;
      align-items: center;
      justify-content: center;
      gap: 6px;
      transition: transform 0.15s, background 0.15s;
    }
    .btn-ctrl:hover { background: #1A1D24; }
    .btn-ctrl:active { transform: scale(0.97); }

    .btn-play-pause {
      width: 42px;
      height: 42px;
      border-radius: 50%;
      background: linear-gradient(135deg, rgba(0, 195, 227, 0.35), rgba(255, 60, 40, 0.35));
      border: 1px solid var(--border-dark-solid);
      color: #FFFFFF;
      font-size: 16px;
      flex-shrink: 0;
    }

    /* Rodapé do Card Frontal */
    .card-footer {
      display: flex;
      flex-direction: column;
      gap: 4px;
      padding-top: 6px;
      border-top: 1px solid #1F232D;
      width: 100%;
    }
    .card-footer-ip {
      font-size: 18px;
      font-weight: 700;
      color: #6DD58C;
      text-shadow: 0 2px 4px var(--shadow-black);
    }
    .card-footer-tip {
      font-size: 13px;
      color: #8E918F;
      text-shadow: 0 1px 2px var(--shadow-black);
    }

    /* Backdrop de Desfoque Leve + Símbolos DVD Bouncing */
    @keyframes symbols-drift-bg {
      0% { background-position: 0px 0px, 0px 0px; }
      100% { background-position: 52px -52px, 104px -104px; }
    }

    .menu-backdrop-blur {
      position: fixed;
      top: 0;
      left: 0;
      width: 100vw;
      height: 100vh;
      background: rgba(4, 6, 10, 0.32);
      backdrop-filter: blur(3.5px) saturate(115%);
      -webkit-backdrop-filter: blur(3.5px) saturate(115%);
      transition: opacity 0.34s ease, visibility 0.34s ease;
      opacity: 1;
      visibility: visible;
      z-index: 80;
      overflow: hidden;
      cursor: pointer;
    }
    .menu-backdrop-blur.closed {
      opacity: 0;
      visibility: hidden;
      pointer-events: none;
      transition: opacity 0.25s ease, visibility 0.25s ease;
    }

    .dvd-geo-symbol {
      position: absolute;
      left: 0;
      top: 0;
      width: 34px;
      height: 34px;
      filter: drop-shadow(0 4px 8px #000000) drop-shadow(0 2px 4px #000000);
      opacity: 0;
      transition: opacity 0.45s ease 0.18s, color 0.25s ease;
      pointer-events: none;
      will-change: transform;
    }
    .menu-backdrop-blur:not(.closed) .dvd-geo-symbol {
      opacity: 0.85;
    }

    /* Vitrais geométricos de fundo */
    .stained-glass-shard-blue {
      position: absolute;
      top: -60px;
      left: -40px;
      width: 380px;
      height: 380px;
      background: radial-gradient(circle, rgba(0,195,227,0.35) 0%, rgba(0,96,122,0.18) 50%, transparent 75%);
      clip-path: polygon(50% 0%, 100% 50%, 50% 100%, 0% 50%);
      filter: blur(22px) drop-shadow(0 8px 18px #000000);
      pointer-events: none;
      z-index: 1;
    }
    .stained-glass-shard-red {
      position: absolute;
      top: -50px;
      right: -40px;
      width: 380px;
      height: 380px;
      background: radial-gradient(circle, rgba(255,60,40,0.35) 0%, rgba(122,20,10,0.18) 50%, transparent 75%);
      clip-path: polygon(50% 0%, 100% 50%, 50% 100%, 0% 50%);
      filter: blur(24px) drop-shadow(0 8px 18px #000000);
      pointer-events: none;
      z-index: 1;
    }

    /* Gaveta Lateral Deslizante (drawerW = 420px) */
    .drawer-panel {
      position: fixed;
      top: 0;
      left: 0;
      width: 420px;
      max-width: 85vw;
      height: 100vh;
      background: rgba(10, 12, 16, 0.97);
      border-right: 1.5px solid var(--border-dark-solid);
      box-shadow: 20px 0 50px var(--shadow-black);
      z-index: 90;
      transform: translateX(-100%);
      transition: transform 0.68s cubic-bezier(0.22, 1, 0.36, 1);
      display: flex;
      flex-direction: column;
      padding: 24px 20px;
    }
    .drawer-panel.open {
      transform: translateX(0);
    }

    /* Abas em Linha Única no Topo da Gaveta */
    .drawer-tabs-row {
      display: flex;
      gap: 6px;
      margin-left: 68px; /* Espaço para não cobrir o botão Morph */
      margin-bottom: 24px;
      white-space: nowrap;
      flex-wrap: nowrap;
    }
    .drawer-tab-btn {
      flex: 1;
      height: 34px;
      padding: 0 10px;
      border-radius: 8px;
      border: 1px solid var(--border-dark-deep);
      background: #10131A;
      color: #9CA3AF;
      font-size: 13px;
      font-weight: 700;
      cursor: pointer;
      display: flex;
      align-items: center;
      justify-content: center;
      white-space: nowrap;
      text-shadow: 0 1px 2px var(--shadow-black);
      transition: all 0.2s ease;
    }
    .drawer-tab-btn.active {
      background: #1B212D;
      border-color: var(--switch-blue);
      color: var(--switch-blue);
      box-shadow: 0 0 10px rgba(0, 195, 227, 0.25);
    }

    .drawer-content {
      flex: 1;
      overflow-y: auto;
      display: flex;
      flex-direction: column;
      gap: 16px;
    }

    .drawer-section-title {
      font-size: 15px;
      font-weight: 700;
      letter-spacing: 0.5px;
      text-shadow: 0 1px 2px var(--shadow-black);
    }

    .guide-card-step {
      background: rgba(0, 0, 0, 0.65);
      border: 1px solid var(--border-dark-deep);
      border-radius: 12px;
      padding: 12px 14px;
      display: flex;
      flex-direction: column;
      gap: 4px;
    }
    .guide-card-step .step-num {
      font-size: 12px;
      font-weight: 700;
      color: var(--switch-blue);
    }
    .guide-card-step .step-title {
      font-size: 14px;
      font-weight: 600;
      color: #F4F4F5;
    }
    .guide-card-step .step-desc {
      font-size: 12.5px;
      color: var(--text-muted);
      line-height: 1.4;
    }

    .about-item-row {
      display: flex;
      flex-direction: column;
      gap: 2px;
      padding: 8px 12px;
      background: rgba(0, 0, 0, 0.5);
      border-radius: 10px;
      border: 1px solid var(--border-dark-deep);
    }
    .about-item-label { font-size: 11px; color: #8E918F; }
    .about-item-value { font-size: 13.5px; font-weight: 600; color: #F4F4F5; }

    /* Rodapé da Gaveta */
    .drawer-footer {
      display: flex;
      align-items: center;
      justify-content: space-between;
      padding-top: 14px;
      border-top: 1px solid var(--border-dark-deep);
      font-size: 13.5px;
      font-weight: 700;
      color: var(--text-muted);
      white-space: nowrap;
    }
    .drawer-footer-comp {
      color: var(--switch-blue);
      text-shadow: 0 0 8px rgba(0, 195, 227, 0.4);
    }

    /* Toast */
    #toast {
      position: fixed;
      bottom: 24px;
      left: 50%;
      transform: translateX(-50%) translateY(100px);
      background: #11141C;
      color: #FFFFFF;
      border: 1.5px solid var(--border-dark-solid);
      padding: 10px 20px;
      border-radius: 30px;
      font-size: 13px;
      font-weight: 700;
      box-shadow: 0 16px 36px var(--shadow-black);
      opacity: 0;
      transition: transform 0.3s cubic-bezier(0.18, 0.89, 0.32, 1.28), opacity 0.3s;
      z-index: 999;
      pointer-events: none;
      text-shadow: 0 2px 4px var(--shadow-black);
    }
    #toast.show {
      transform: translateX(-50%) translateY(0);
      opacity: 1;
    }
  </style>
</head>
<body>

  <!-- Video Wallpaper Animado -->
  <video id="bgVideo" class="bg-video" autoplay loop muted playsinline poster="/icon.jpg">
    <source src="/wallpaper.mp4" type="video/mp4">
  </video>

  <!-- Botão Morph (Hambúrguer -> Nintendo Switch) -->
  <button id="morphBtn" class="morph-btn" onclick="toggleMenu()" aria-label="Menu SwitchCast">
    <div class="morph-stage">
      <span class="hbar hbar-1"></span>
      <span class="hbar hbar-2"></span>
      <span class="hbar hbar-3"></span>
    </div>
  </button>

  <!-- Backdrop de Blur Leve e 4 Símbolos Estilo DVD Player (Batem nos cantos e entre si) -->
  <div id="menuBackdrop" class="menu-backdrop-blur closed" onclick="toggleMenu(false)">
    <svg id="dvdSym0" class="dvd-geo-symbol" style="color:#00C3E3;" viewBox="0 0 24 24" fill="currentColor">
      <path d="M12 2l10 10-10 10L2 12z"/>
    </svg>
    <svg id="dvdSym1" class="dvd-geo-symbol" style="color:#FF3C28;" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.8">
      <circle cx="12" cy="12" r="8"/>
    </svg>
    <svg id="dvdSym2" class="dvd-geo-symbol" style="color:#E4E4E7;" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.7">
      <rect x="4" y="4" width="16" height="16" rx="2.5"/>
    </svg>
    <svg id="dvdSym3" class="dvd-geo-symbol" style="color:#00C3E3;" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.7">
      <path d="M12 3.5l9.5 16.5H2.5z"/>
    </svg>
  </div>

  <!-- Gaveta Lateral Deslizante -->
  <div id="drawerPanel" class="drawer-panel">
    <!-- Abas em Linha Única -->
    <div class="drawer-tabs-row">
      <button id="tabBtnHow" class="drawer-tab-btn active" onclick="switchDrawerTab('how')">Como Usar</button>
      <button id="tabBtnNews" class="drawer-tab-btn" onclick="switchDrawerTab('news')">Novidades</button>
      <button id="tabBtnAbout" class="drawer-tab-btn" onclick="switchDrawerTab('about')">Sobre</button>
    </div>

    <!-- Conteúdo da Aba Como Usar -->
    <div id="drawerTabHow" class="drawer-content">
      <div class="drawer-section-title" style="color:var(--switch-blue);">GUIA R&Aacute;PIDO DE USO</div>
      <div class="guide-card-step">
        <span class="step-num">1. Conex&atilde;o Wi-Fi</span>
        <span class="step-title">Mesma Rede Local</span>
        <span class="step-desc">Conecte celular, PC ou tablet na mesma rede Wi-Fi deste Switch.</span>
      </div>
      <div class="guide-card-step">
        <span class="step-num">2. Selecione a M&iacute;dia</span>
        <span class="step-title">Seu App Preferido</span>
        <span class="step-desc">Use Web Video Caster, BubbleUPnP, YouTube, VLC ou navegadores.</span>
      </div>
      <div class="guide-card-step">
        <span class="step-num">3. Transmitir para o Switch</span>
        <span class="step-title">Toque no &Iacute;cone Cast / DLNA</span>
        <span class="step-desc">Escolha "SwitchCast" na lista de dispositivos para reproduzir.</span>
      </div>
      <div class="guide-card-step">
        <span class="step-num">4. Controle Remoto Web</span>
        <span class="step-title">Navegador Local</span>
        <span class="step-desc">Acesse este painel para play/pause, seek, volume a 150% e legendas.</span>
      </div>
    </div>

    <!-- Conteúdo da Aba Novidades -->
    <div id="drawerTabNews" class="drawer-content" style="display:none;">
      <div class="drawer-section-title" style="color:var(--switch-red);">NOVIDADES NA VERS&Atilde;O 2.3</div>
      <div class="guide-card-step">
        <span class="step-title" style="color:#F4F4F5;">Interface Minimalista</span>
        <span class="step-desc">Painel frontal centralizado "Vamos dar o play?" focado no essencial.</span>
      </div>
      <div class="guide-card-step">
        <span class="step-title" style="color:#F4F4F5;">Anima&ccedil;&atilde;o Switch Morph</span>
        <span class="step-desc">Metamorfose fluida das 3 barras do hamb&uacute;rguer formando o Switch.</span>
      </div>
      <div class="guide-card-step">
        <span class="step-title" style="color:#F4F4F5;">S&iacute;mbolos DVD Bouncing</span>
        <span class="step-desc">Colis&otilde;es el&aacute;sticas e drifting flutuante ao abrir a gaveta.</span>
      </div>
      <div class="guide-card-step">
        <span class="step-title" style="color:#F4F4F5;">Servidor FTP Ultrarr&aacute;pido</span>
        <span class="step-desc">Acesso direto ao cart&atilde;o microSD nas portas 5000 e 21.</span>
      </div>
    </div>

    <!-- Conteúdo da Aba Sobre -->
    <div id="drawerTabAbout" class="drawer-content" style="display:none;">
      <div class="drawer-section-title" style="color:#6DD58C;">SOBRE O SWITCHCAST</div>
      <div class="about-item-row">
        <span class="about-item-label">Sistema Operacional</span>
        <span class="about-item-value">Atmosph&egrave;re CFW</span>
      </div>
      <div class="about-item-row" style="cursor:pointer;" onclick="toggleOtaChannel()">
        <span class="about-item-label">Canal OTA (Clique p/ alternar)</span>
        <span class="about-item-value" id="otaChannelLabel" style="color:var(--switch-blue);">Est&aacute;vel (main)</span>
      </div>
      <div class="about-item-row">
        <span class="about-item-label">Decodificador</span>
        <span class="about-item-value">Nvidia Tegra X1 NVDEC HW (60 FPS)</span>
      </div>
      <div class="about-item-row">
        <span class="about-item-label">&Aacute;udio</span>
        <span class="about-item-value">Soft Limiter + ITU-R BS.775 5.1 Downmix</span>
      </div>
      <div class="about-item-row">
        <span class="about-item-label">Protocolos Suportados</span>
        <span class="about-item-value">DLNA / UPnP, SSDP, WebSocket &amp; REST</span>
      </div>
      <div class="about-item-row">
        <span class="about-item-label">Servidor FTP</span>
        <span class="about-item-value">Portas 5000 / 21 ativas</span>
      </div>
      <div class="about-item-row">
        <span class="about-item-label">Desenvolvedor</span>
        <span class="about-item-value">SwitchCast Team</span>
      </div>
    </div>

    <!-- Rodapé da Gaveta -->
    <div class="drawer-footer">
      <span>v2.4.0</span>
      <span class="drawer-footer-comp">Comp. 30</span>
    </div>
  </div>

  <!-- Tela 1: Base Screen -->
  <div class="base-screen">
    <!-- Vitrais Facetados Intermediários -->
    <div class="stained-glass-shard-blue"></div>
    <div class="stained-glass-shard-red"></div>

    <!-- Badge Superior Direito -->
    <div class="top-right-badge">
      <div class="dot"></div>
      <span>v2.4.0 | Comp. 30</span>
    </div>

    <!-- Padrão Micro PlayStation -->
    <svg class="ps-texture" width="100%" height="100%">
      <defs>
        <pattern id="psGrid" width="48" height="48" patternUnits="userSpaceOnUse">
          <text x="8" y="16" fill="rgba(255,255,255,0.06)" font-size="11" font-weight="bold">△</text>
          <text x="32" y="16" fill="rgba(0,195,227,0.08)" font-size="11" font-weight="bold">○</text>
          <text x="8" y="40" fill="rgba(255,60,40,0.08)" font-size="11" font-weight="bold">✕</text>
          <text x="32" y="40" fill="rgba(255,255,255,0.06)" font-size="11" font-weight="bold">□</text>
        </pattern>
      </defs>
      <rect width="100%" height="100%" fill="url(#psGrid)" />
    </svg>

    <!-- Cenário Alvo de Desfoque -->
    <div id="bgSceneTarget" class="bg-scene-blur-target">
      <!-- Tela 2 Frontal ("Vamos dar o play?") -->
      <div class="front-panel">
        <div class="panel-brand-title">SwitchCast</div>

        <div class="play-icon-box" onclick="togglePlayPause()" title="Iniciar / Alternar Reprodução">
          &#9658;
        </div>

        <div class="hero-title">Vamos dar o play?</div>
        <div class="hero-subtitle">Envie v&iacute;deos ou m&uacute;sicas do seu celular ou computador para reproduzir instantaneamente.</div>

        <!-- Barra de Transmissão por URL -->
        <div class="cast-bar">
          <input type="text" id="mediaUrl" class="cast-input" placeholder="Cole a URL do vídeo ou stream (MP4, MKV, M3U8...)">
          <button class="cast-btn" onclick="castMedia()">
            &#9658; Transmitir
          </button>
        </div>

        <!-- Widget de Reprodução Ativa -->
        <div class="playback-widget">
          <div class="playback-info-row">
            <span id="mediaTitle" class="playback-title">Nenhuma m&iacute;dia ativa</span>
            <span id="mediaState" class="playback-state-pill">OCIOSO</span>
          </div>

          <input type="range" id="seekSlider" class="seek-slider" min="0" max="100" value="0">
          <div class="time-labels">
            <span id="currentTime">00:00</span>
            <span id="totalDuration">00:00</span>
          </div>

          <div class="controls-row">
            <button class="btn-ctrl" onclick="sendCmd('seek', {offsetMs: -10000})">&#9194; -10s</button>
            <button id="btnPlayPause" class="btn-ctrl btn-play-pause" onclick="togglePlayPause()">&#9654;</button>
            <button class="btn-ctrl" onclick="sendCmd('stop')">&#9209; Parar</button>
            <button class="btn-ctrl" onclick="sendCmd('seek', {offsetMs: 10000})">+10s &#9193;</button>
          </div>

          <div style="display:flex; align-items:center; gap:10px; width:100%; padding-top:4px;">
            <span style="color:var(--switch-blue); font-size:14px;">&#128266;</span>
            <input type="range" id="volSlider" class="seek-slider" min="0" max="150" value="80" oninput="onVolumeChange(this.value)">
            <span id="volLabel" style="font-size:12px; font-weight:700; color:var(--switch-blue); min-width:40px; text-align:right;">80%</span>
            <button id="btnMute" class="btn-ctrl" style="height:28px; padding:0 8px;" onclick="toggleMute()">Mudo</button>
          </div>
        </div>

        <!-- Rodapé do Card -->
        <div class="card-footer">
          <div id="footerIp" class="card-footer-ip">Conectando ao Switch...</div>
          <div class="card-footer-tip">Abra no navegador para controle remoto instant&acirc;neo</div>
        </div>
      </div>
    </div>
  </div>

  <div id="toast">Notificação</div>

  <script>
    let ws = null;
    let isPlaying = false;
    let isMuted = false;
    let isUserSeeking = false;
    let durationMs = 0;
    let menuOpen = false;
    let dvdAnimId = null;

    const dvdColors = ['#00C3E3', '#FF3C28', '#E4E4E7', '#38BDF8', '#FB7185'];
    const SYM_SIZE = 34;
    const SYM_RADIUS = 17;

    const dvdSymbols = [
      { id: 'dvdSym0', x: 460, y: 80,  vx:  2.35, vy:  1.95, colorIdx: 0, scale: 1 },
      { id: 'dvdSym1', x: 720, y: 140, vx: -2.15, vy:  2.20, colorIdx: 1, scale: 1 },
      { id: 'dvdSym2', x: 520, y: 380, vx:  2.25, vy: -2.05, colorIdx: 2, scale: 1 },
      { id: 'dvdSym3', x: 780, y: 420, vx: -2.20, vy: -1.90, colorIdx: 0, scale: 1 }
    ];

    function pulseSymbolColor(sym) {
      sym.colorIdx = (sym.colorIdx + 1) % dvdColors.length;
      sym.scale = 1.24;
      const el = document.getElementById(sym.id);
      if (el) el.style.color = dvdColors[sym.colorIdx];
    }

    function updateDvdPhysics() {
      if (!menuOpen) return;
      const W = window.innerWidth || 1280;
      const H = window.innerHeight || 720;
      const minX = Math.min(426, W * 0.45);
      const maxX = Math.max(minX + 100, W - SYM_SIZE - 8);
      const minY = 8;
      const maxY = H - SYM_SIZE - 8;

      for (let i = 0; i < dvdSymbols.length; i++) {
        const s = dvdSymbols[i];
        s.x += s.vx;
        s.y += s.vy;
        let hitWall = false;
        if (s.x <= minX) { s.x = minX; s.vx = Math.abs(s.vx); hitWall = true; }
        else if (s.x >= maxX) { s.x = maxX; s.vx = -Math.abs(s.vx); hitWall = true; }
        if (s.y <= minY) { s.y = minY; s.vy = Math.abs(s.vy); hitWall = true; }
        else if (s.y >= maxY) { s.y = maxY; s.vy = -Math.fabs ? -Math.abs(s.vy) : -Math.abs(s.vy); hitWall = true; }
        if (hitWall) pulseSymbolColor(s);
      }

      const minDist = SYM_RADIUS * 2;
      for (let i = 0; i < dvdSymbols.length; i++) {
        for (let j = i + 1; j < dvdSymbols.length; j++) {
          const a = dvdSymbols[i];
          const b = dvdSymbols[j];
          const dx = (b.x + SYM_RADIUS) - (a.x + SYM_RADIUS);
          const dy = (b.y + SYM_RADIUS) - (a.y + SYM_RADIUS);
          const dist = Math.hypot(dx, dy);
          if (dist < minDist && dist > 0.001) {
            const nx = dx / dist;
            const ny = dy / dist;
            const overlap = (minDist - dist) * 0.5;
            a.x -= nx * overlap; a.y -= ny * overlap;
            b.x += nx * overlap; b.y += ny * overlap;
            const dvx = a.vx - b.vx;
            const dvy = a.vy - b.vy;
            const dot = dvx * nx + dvy * ny;
            if (dot > 0) {
              a.vx -= dot * nx; a.vy -= dot * ny;
              b.vx += dot * nx; b.vy += dot * ny;
              pulseSymbolColor(a);
              pulseSymbolColor(b);
            }
          }
        }
      }

      for (let i = 0; i < dvdSymbols.length; i++) {
        const s = dvdSymbols[i];
        s.scale += (1.0 - s.scale) * 0.15;
        const el = document.getElementById(s.id);
        if (el) {
          el.style.transform = `translate3d(${s.x.toFixed(1)}px, ${s.y.toFixed(1)}px, 0) scale(${s.scale.toFixed(3)})`;
        }
      }

      dvdAnimId = requestAnimationFrame(updateDvdPhysics);
    }

    function toggleMenu(forceState) {
      menuOpen = (forceState !== undefined) ? forceState : !menuOpen;
      const morphBtn = document.getElementById('morphBtn');
      const backdrop = document.getElementById('menuBackdrop');
      const drawer = document.getElementById('drawerPanel');
      const sceneTarget = document.getElementById('bgSceneTarget');

      if (menuOpen) {
        morphBtn.classList.add('is-switch');
        backdrop.classList.remove('closed');
        drawer.classList.add('open');
        sceneTarget.classList.add('is-blurred');
        if (!dvdAnimId) dvdAnimId = requestAnimationFrame(updateDvdPhysics);
      } else {
        morphBtn.classList.remove('is-switch');
        backdrop.classList.add('closed');
        drawer.classList.remove('open');
        sceneTarget.classList.remove('is-blurred');
        if (dvdAnimId) {
          cancelAnimationFrame(dvdAnimId);
          dvdAnimId = null;
        }
      }
    }

    function switchDrawerTab(tab) {
      document.getElementById('drawerTabHow').style.display = (tab === 'how') ? 'flex' : 'none';
      document.getElementById('drawerTabNews').style.display = (tab === 'news') ? 'flex' : 'none';
      document.getElementById('drawerTabAbout').style.display = (tab === 'about') ? 'flex' : 'none';

      document.getElementById('tabBtnHow').classList.toggle('active', tab === 'how');
      document.getElementById('tabBtnNews').classList.toggle('active', tab === 'news');
      document.getElementById('tabBtnAbout').classList.toggle('active', tab === 'about');
    }

    function showToast(msg) {
      const t = document.getElementById('toast');
      t.innerText = msg;
      t.classList.add('show');
      setTimeout(() => { t.classList.remove('show'); }, 2200);
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
      document.getElementById('footerIp').innerText = `http://${host}:${port}`;
      
      try {
        ws = new WebSocket(wsUrl);

        ws.onopen = () => {
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

      titleEl.innerText = (s.title && s.title.length > 0) ? s.title : (s.state !== 'Idle' ? "Transmissão Ativa" : "Nenhuma mídia ativa");
      durationMs = s.durationMs || 0;

      if (s.state === 'Playing') {
        isPlaying = true;
        stateEl.innerText = "REPRODUZINDO";
        stateEl.className = "playback-state-pill playing";
        btnPlay.innerHTML = "&#10074;&#10074;";
      } else if (s.state === 'Paused') {
        isPlaying = false;
        stateEl.innerText = "PAUSADO";
        stateEl.className = "playback-state-pill";
        btnPlay.innerHTML = "&#9654;";
      } else {
        isPlaying = false;
        stateEl.innerText = "OCIOSO";
        stateEl.className = "playback-state-pill";
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
        document.getElementById('btnMute').innerText = isMuted ? "Mudo (On)" : "Mudo";
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
      showToast(isMuted ? "Mudo ativado" : "Som ativado");
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
      if (!url) {
        showToast("Por favor, digite ou cole a URL da mídia.");
        return;
      }
      sendCmd('play', {url: url, title: "Transmissão Web"});
      showToast("Enviando mídia para o Switch...");
    }

    function fetchOtaChannel() {
      fetch('/api/v1/ota/channel')
        .then(r => r.json())
        .then(data => {
          if (data && data.ok) {
            const el = document.getElementById('otaChannelLabel');
            if (el) {
              el.innerText = data.name + (data.channel === 'beta' ? ' (dev branch)' : ' (main branch)');
              el.style.color = (data.channel === 'beta') ? 'var(--switch-red)' : 'var(--switch-blue)';
            }
          }
        }).catch(() => {});
    }

    function toggleOtaChannel() {
      fetch('/api/v1/ota/channel', {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({})
      })
      .then(r => r.json())
      .then(data => {
        if (data && data.ok) {
          const el = document.getElementById('otaChannelLabel');
          if (el) {
            el.innerText = data.name + (data.channel === 'beta' ? ' (dev branch)' : ' (main branch)');
            el.style.color = (data.channel === 'beta') ? 'var(--switch-red)' : 'var(--switch-blue)';
          }
          showToast("Canal alterado: " + data.name);
        }
      }).catch(() => {
        showToast("Erro ao alternar canal");
      });
    }

    setInterval(() => {
      if (!ws || ws.readyState !== WebSocket.OPEN) {
        fetch('/api/v1/status')
          .then(r => r.json())
          .then(data => updateUI(data))
          .catch(() => {});
      }
    }, 1500);

    connectWs();
    fetchOtaChannel();
  </script>
</body>
</html>
)HTML_CONTENT";
}

}  // namespace SwitchCast
