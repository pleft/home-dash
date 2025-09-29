// Home Dashboard - Live-only Web App (no history, no charts)

// DOM elements
const sensorCards = document.getElementById('sensorCards');

// Clock elements
const currentTimeEl = document.getElementById('currentTime');
const currentDateEl = document.getElementById('currentDate');
const timezoneEl = document.getElementById('timezone');

// ESP32 system info elements
const esp32DomainEl = document.getElementById('esp32Domain');
const esp32IPEl = document.getElementById('esp32IP');
const esp32HeapEl = document.getElementById('esp32Heap');
const esp32UptimeEl = document.getElementById('esp32Uptime');
const esp32WifiModeEl = document.getElementById('esp32WifiMode');
const esp32HttpReqsEl = document.getElementById('esp32HttpReqs');
const esp32BleAdvertsEl = document.getElementById('esp32BleAdverts');
const esp32CpuUsageEl = document.getElementById('esp32CpuUsage');

// ================= CLOCK =================
function updateClock() {
  const now = new Date();
  const timeStr = now.toLocaleTimeString('en-US', { hour12: false, hour: '2-digit', minute: '2-digit', second: '2-digit' });
  const dateStr = now.toLocaleDateString('en-US', { weekday: 'long', year: 'numeric', month: 'long', day: 'numeric' });
  const timezoneStr = Intl.DateTimeFormat().resolvedOptions().timeZone;
  if (currentTimeEl) currentTimeEl.textContent = timeStr;
  if (currentDateEl) currentDateEl.textContent = dateStr;
  if (timezoneEl) timezoneEl.textContent = timezoneStr;
  // Update greeting and background
  updateGreetingAndBackground(now);
}
let clockInterval = setInterval(updateClock, 1000);
updateClock();

// ================= ESP32 INFO =================
async function updateESP32Info() {
  try {
    const response = await fetch('/health', { cache: 'no-store' });
    if (!response.ok) return;
    const data = await response.json();

    if (esp32DomainEl) {
      const hostname = window.location.hostname;
      const port = window.location.port ? `:${window.location.port}` : '';
      esp32DomainEl.textContent = `${hostname}${port}`;
    }
    if (esp32IPEl) esp32IPEl.textContent = data.ip || 'Unknown';
    if (esp32HeapEl && data.heap) esp32HeapEl.textContent = `${Math.round(data.heap / 1024)} KB`;
    if (esp32UptimeEl && data.uptime_ms != null) {
      const s = Math.floor(data.uptime_ms / 1000);
      const h = Math.floor(s / 3600);
      const m = Math.floor((s % 3600) / 60);
      const sec = s % 60;
      esp32UptimeEl.textContent = `${h}h ${m}m ${sec}s`;
    }
    if (esp32WifiModeEl && data.ap_mode !== undefined) esp32WifiModeEl.textContent = data.ap_mode ? 'AP Mode' : 'STA Mode';
    if (esp32HttpReqsEl && data.req_total !== undefined) esp32HttpReqsEl.textContent = String(data.req_total);
    if (esp32BleAdvertsEl && data.ble_seen !== undefined) esp32BleAdvertsEl.textContent = String(data.ble_seen);
    if (esp32CpuUsageEl && data.cpu_usage !== undefined) esp32CpuUsageEl.textContent = `${Number(data.cpu_usage).toFixed(1)}%`;
  } catch {}
}
setInterval(updateESP32Info, 20000);
updateESP32Info();
// Initialize visual background immediately
initializeESP32Background();

function updateESP32Background(data) {
  const el = document.getElementById('esp32Background');
  if (!el) return;
  el.innerHTML = createCircuitBackground(data || { heap: 0, uptime_ms: 0, ap_mode: false });
}

// Provide an initial background before first /health response
function initializeESP32Background() {
  const backgroundEl = document.getElementById('esp32Background');
  if (!backgroundEl) return;
  const initialData = { heap: 25000, uptime_ms: 1000, ap_mode: false };
  backgroundEl.innerHTML = createCircuitBackground(initialData);
}

function createCircuitBackground(data) {
  const heapOK = (data.heap || 0) > 20000;
  const wifiOK = !data.ap_mode;
  return `
    <svg viewBox="0 0 400 200" xmlns="http://www.w3.org/2000/svg">
      <defs>
        <linearGradient id="circuitBg" x1="0%" y1="0%" x2="100%" y2="100%">
          <stop offset="0%" style="stop-color:#0ea5e9;stop-opacity:0.1" />
          <stop offset="100%" style="stop-color:#0284c7;stop-opacity:0.2" />
        </linearGradient>
        <filter id="glow"><feGaussianBlur stdDeviation="2" result="b"/><feMerge><feMergeNode in="b"/><feMergeNode in="SourceGraphic"/></feMerge></filter>
      </defs>
      <rect width="400" height="200" fill="url(#circuitBg)"/>
      <rect x="150" y="80" width="100" height="60" rx="8" fill="rgba(255,255,255,0.2)" stroke="rgba(255,255,255,0.4)" stroke-width="2"/>
      <text x="200" y="105" text-anchor="middle" fill="rgba(255,255,255,0.85)" font-size="12" font-family="monospace">ESP32</text>
      <g stroke="rgba(255,255,255,0.6)" stroke-width="2" fill="none">
        <line x1="50" y1="50" x2="150" y2="50"/>
        <line x1="250" y1="50" x2="350" y2="50"/>
        <line x1="50" y1="150" x2="150" y2="150"/>
        <line x1="250" y1="150" x2="350" y2="150"/>
        <line x1="50" y1="100" x2="150" y2="100"/>
        <line x1="250" y1="100" x2="350" y2="100"/>
        <line x1="200" y1="50" x2="200" y2="80"/>
        <line x1="200" y1="140" x2="200" y2="150"/>
      </g>
      <circle cx="100" cy="50" r="3" fill="${heapOK ? '#10b981' : '#ef4444'}" filter="url(#glow)"/>
      <circle cx="300" cy="50" r="3" fill="${wifiOK ? '#10b981' : '#f59e0b'}" filter="url(#glow)"/>
      <circle cx="100" cy="150" r="3" fill="#6366f1" filter="url(#glow)"/>
      <circle cx="300" cy="150" r="3" fill="#8b5cf6" filter="url(#glow)"/>
    </svg>
  `;
}

// ================= THEME =================
function initializeThemeToggle() {
  const themeToggle = document.getElementById('themeToggle');
  const themeIcon = themeToggle?.querySelector('.theme-icon');
  if (!themeToggle || !themeIcon) return;

  const savedTheme = localStorage.getItem('dashboard-theme') || 'dark';
  document.documentElement.setAttribute('data-theme', savedTheme);
  updateThemeIcon(themeIcon, savedTheme);

  themeToggle.addEventListener('click', () => {
    const currentTheme = document.documentElement.getAttribute('data-theme');
    const newTheme = currentTheme === 'dark' ? 'light' : 'dark';
    document.documentElement.setAttribute('data-theme', newTheme);
    localStorage.setItem('dashboard-theme', newTheme);
    updateThemeIcon(themeIcon, newTheme);
  });
}
function updateThemeIcon(icon, theme) { icon.textContent = theme === 'dark' ? '🌙' : '☀️'; }
initializeThemeToggle();

// ================= LIVE SENSOR CARDS =================
function createSensorCard(tag) {
  const displayName = tag.name && tag.name.length ? tag.name : tag.mac;
  const isOnline = tag.age < 30;
  return `
    <div class="sensor-card" id="card-${tag.mac.replace(/:/g, '')}">
      <div class="sensor-card-header">
        <div class="sensor-name">${displayName}</div>
        <div class="sensor-status ${isOnline ? '' : 'offline'}"></div>
      </div>
      <div class="sensor-metrics">
        <div class="metric"><span class="metric-label">🌡️ Temp:</span><span class="metric-value">${tag.t != null ? Number(tag.t).toFixed(1) + '°C' : 'N/A'}</span></div>
        <div class="metric"><span class="metric-label">💧 Humidity:</span><span class="metric-value">${tag.h != null ? Number(tag.h).toFixed(1) + '%' : 'N/A'}</span></div>
        <div class="metric"><span class="metric-label">🔽 Pressure:</span><span class="metric-value">${tag.p != null ? Number(tag.p / 1000).toFixed(1) + ' kPa' : 'N/A'}</span></div>
        <div class="metric"><span class="metric-label">🔋 Battery:</span><span class="metric-value">${tag.batt != null ? tag.batt + ' mV' : 'N/A'}</span></div>
        <div class="metric"><span class="metric-label">📶 Signal:</span><span class="metric-value">${tag.rssi} dBm</span></div>
        <div class="metric"><span class="metric-label">⏱️ Last Seen:</span><span class="metric-value">${tag.age}s ago</span></div>
      </div>
    </div>
  `;
}
function updateSensorCard(tag) {
  const cardId = `card-${tag.mac.replace(/:/g, '')}`;
  let card = document.getElementById(cardId);
  if (!card) {
    sensorCards.insertAdjacentHTML('beforeend', createSensorCard(tag));
  } else {
    card.outerHTML = createSensorCard(tag);
  }
}

// ================= LIVE POLLING & UI ENHANCEMENTS =================
let inFlight = false;
let pollIntervalMs = 8000;
let timerId = null;
const showOffline = null;
const lastUpdateEl = null;
const liveStatusEl = null;
const refreshBtn = null;

let lastSnapshot = [];

function renderCards(tags) {
  // Filter only
  const filtered = tags;
  sensorCards.innerHTML = '';
  filtered.forEach(updateSensorCard);
}

async function tick() {
  if (inFlight) return; inFlight = true;
  try {
    const r = await fetch('/data', { cache: 'no-store', headers: { 'Accept': 'application/json', 'Cache-Control': 'no-cache', 'Pragma': 'no-cache' } });
    if (!r.ok) throw new Error('HTTP ' + r.status);
    const snap = await r.json();
    if (snap && Array.isArray(snap.tags)) {
      lastSnapshot = snap.tags;
      renderCards(lastSnapshot);
      
    }
    if (pollIntervalMs !== 10000) { pollIntervalMs = 10000; restartTimer(); }
  } catch (e) {
    pollIntervalMs = Math.min(pollIntervalMs * 1.5, 60000);
    
    restartTimer();
  } finally {
    inFlight = false;
  }
}
function restartTimer() { if (timerId) clearInterval(timerId); timerId = setInterval(tick, pollIntervalMs); }

document.addEventListener('visibilitychange', () => {
  if (document.hidden) { if (timerId) clearInterval(timerId), timerId = null; if (clockInterval) clearInterval(clockInterval), clockInterval = null; }
  else { restartTimer(); tick(); if (!clockInterval) { clockInterval = setInterval(updateClock, 1000); updateClock(); } }
});

tick();
restartTimer();

// ================= CLOCK BACKGROUNDS & GREETING =================
function updateGreetingAndBackground(now) {
  const hour = now.getHours();
  const greetingEl = document.getElementById('clockGreeting');
  const backgroundEl = document.getElementById('clockBackground');
  if (!greetingEl || !backgroundEl) return;

  let greeting, icon, backgroundSvg;
  if (hour >= 5 && hour < 12) {
    greeting = 'Good Morning';
    icon = '🌅';
    backgroundSvg = createSunriseBackground();
  } else if (hour >= 12 && hour < 17) {
    greeting = 'Good Afternoon';
    icon = '☀️';
    backgroundSvg = createSunBackground();
  } else if (hour >= 17 && hour < 21) {
    greeting = 'Good Evening';
    icon = '🌅';
    backgroundSvg = createSunsetBackground();
  } else {
    greeting = 'Good Night';
    icon = '🌙';
    backgroundSvg = createMoonBackground();
  }
  greetingEl.textContent = `${icon} ${greeting}`;
  backgroundEl.innerHTML = backgroundSvg;
}

function createSunriseBackground() {
  return `
    <svg viewBox="0 0 400 200" xmlns="http://www.w3.org/2000/svg">
      <defs>
        <linearGradient id="sunriseSky" x1="0%" y1="0%" x2="0%" y2="100%">
          <stop offset="0%" style="stop-color:#ff6b6b;stop-opacity:1" />
          <stop offset="50%" style="stop-color:#ffa726;stop-opacity:1" />
          <stop offset="100%" style="stop-color:#4fc3f7;stop-opacity:1" />
        </linearGradient>
        <radialGradient id="sun" cx="50%" cy="30%" r="20%">
          <stop offset="0%" style="stop-color:#fff59d;stop-opacity:1" />
          <stop offset="100%" style="stop-color:#ffa726;stop-opacity:0.8" />
        </radialGradient>
      </defs>
      <rect width="400" height="200" fill="url(#sunriseSky)"/>
      <circle cx="200" cy="60" r="25" fill="url(#sun)" opacity="0.9"/>
      <g stroke="#fff59d" stroke-width="2" opacity="0.6">
        <line x1="200" y1="20" x2="200" y2="10"/>
        <line x1="230" y1="35" x2="240" y2="30"/>
        <line x1="240" y1="60" x2="250" y2="60"/>
        <line x1="230" y1="85" x2="240" y2="90"/>
        <line x1="200" y1="100" x2="200" y2="110"/>
        <line x1="170" y1="85" x2="160" y2="90"/>
        <line x1="160" y1="60" x2="150" y2="60"/>
        <line x1="170" y1="35" x2="160" y2="30"/>
      </g>
      <polygon points="0,200 50,150 100,180 150,120 200,160 250,100 300,140 350,110 400,150 400,200" fill="#2e7d32" opacity="0.7"/>
    </svg>
  `;
}

function createSunBackground() {
  return `
    <svg viewBox="0 0 400 200" xmlns="http://www.w3.org/2000/svg">
      <defs>
        <linearGradient id="daySky" x1="0%" y1="0%" x2="0%" y2="100%">
          <stop offset="0%" style="stop-color:#87ceeb;stop-opacity:1" />
          <stop offset="100%" style="stop-color:#98fb98;stop-opacity:1" />
        </linearGradient>
        <radialGradient id="sunDay" cx="50%" cy="20%" r="25%">
          <stop offset="0%" style="stop-color:#ffff00;stop-opacity:1" />
          <stop offset="100%" style="stop-color:#ffa500;stop-opacity:0.8" />
        </radialGradient>
      </defs>
      <rect width="400" height="200" fill="url(#daySky)"/>
      <circle cx="200" cy="40" r="30" fill="url(#sunDay)" opacity="0.9"/>
      <ellipse cx="100" cy="50" rx="25" ry="15" fill="white" opacity="0.8"/>
      <ellipse cx="300" cy="60" rx="30" ry="18" fill="white" opacity="0.8"/>
    </svg>
  `;
}

function createSunsetBackground() {
  return `
    <svg viewBox="0 0 400 200" xmlns="http://www.w3.org/2000/svg">
      <defs>
        <linearGradient id="sunsetSky" x1="0%" y1="0%" x2="0%" y2="100%">
          <stop offset="0%" style="stop-color:#ff8a65;stop-opacity:1" />
          <stop offset="50%" style="stop-color:#ff7043;stop-opacity:1" />
          <stop offset="100%" style="stop-color:#5d4037;stop-opacity:1" />
        </linearGradient>
        <radialGradient id="sunsetSun" cx="50%" cy="40%" r="20%">
          <stop offset="0%" style="stop-color:#ffcc80;stop-opacity:1" />
          <stop offset="100%" style="stop-color:#ff7043;stop-opacity:0.8" />
        </radialGradient>
      </defs>
      <rect width="400" height="200" fill="url(#sunsetSky)"/>
      <circle cx="200" cy="80" r="25" fill="url(#sunsetSun)" opacity="0.9"/>
      <polygon points="0,200 50,150 100,180 150,120 200,160 250,100 300,140 350,110 400,150 400,200" fill="#3e2723" opacity="0.8"/>
    </svg>
  `;
}

function createMoonBackground() {
  return `
    <svg viewBox="0 0 400 200" xmlns="http://www.w3.org/2000/svg">
      <defs>
        <linearGradient id="nightSky" x1="0%" y1="0%" x2="0%" y2="100%">
          <stop offset="0%" style="stop-color:#1a237e;stop-opacity:1" />
          <stop offset="100%" style="stop-color:#000051;stop-opacity:1" />
        </linearGradient>
        <radialGradient id="moon" cx="50%" cy="25%" r="10%">
          <stop offset="0%" style="stop-color:#e8eaf6;stop-opacity:1" />
          <stop offset="100%" style="stop-color:#c5cae9;stop-opacity:0.8" />
        </radialGradient>
      </defs>
      <rect width="400" height="200" fill="url(#nightSky)"/>
      <circle cx="200" cy="50" r="20" fill="url(#moon)" opacity="0.9"/>
      <g fill="white" opacity="0.8">
        <circle cx="100" cy="30" r="1"/>
        <circle cx="300" cy="40" r="1.5"/>
        <circle cx="150" cy="20" r="1"/>
        <circle cx="250" cy="25" r="1"/>
        <circle cx="80" cy="60" r="1"/>
        <circle cx="320" cy="70" r="1.5"/>
        <circle cx="120" cy="80" r="1"/>
        <circle cx="280" cy="90" r="1"/>
      </g>
      <polygon points="0,200 50,150 100,180 150,120 200,160 250,100 300,140 350,110 400,150 400,200" fill="#1a1a1a" opacity="0.9"/>
    </svg>
  `;
}
