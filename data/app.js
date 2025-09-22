/*
  ==============================================================================
  RuuviTag Live Dashboard - JavaScript Application
  ==============================================================================
  
  Main JavaScript application for the RuuviTag dashboard web interface.
  
  Features:
  - Real-time sensor data polling and display
  - Interactive Chart.js visualizations with multiple data types
  - Adaptive time window filtering with server-side history (30s resolution)
  - Mobile-responsive sensor cards and table layouts
  - Live clock and ESP32 system monitoring
  - Server-side history loading with timestamp synchronization
  
  Architecture:
  - Polling-based data updates every 5 seconds
  - Client-side data filtering based on selected time windows
  - Server-side adaptive history storage (2 hours with 30-second resolution)
  - Responsive design with mobile-first approach
  
  Dependencies:
  - Chart.js 4.4.3 (charting library)
  - Chart.js date adapter (time-based charts)
  - ESP32 HTTP server with /data and /history endpoints
  
  Author: AI Assistant
  Version: 2.0
  Last Updated: 2024
  ==============================================================================
*/

// =============================== GLOBAL STATE ===============================
// DOM element references - with null checks
const sensorCards = document.getElementById('sensorCards');
const chartTypeSel = document.getElementById('chartTypeSel');
const windowSel = document.getElementById('windowSel');
const chartModeSel = document.getElementById('chartModeSel');
const chartTitle = document.getElementById('chartTitle');

// Verify critical elements exist
if (!chartModeSel) console.error('chartModeSel element not found');
if (!chartTitle) console.error('chartTitle element not found');

// No separate sensor selector needed - sensors are added directly to chart mode dropdown

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

let windowMs = windowSel ? Number(windowSel.value) : 1800000; // Default 30 minutes
let currentChartType = chartTypeSel ? chartTypeSel.value : 'temperature';
let currentChartMode = chartModeSel ? chartModeSel.value : 'all';

const historyByMac = new Map(); // mac -> { t:[], h:[], p:[] } // Removed batt and rssi to save space
const MAX_POINTS = 3600; // per series (~2 hours @5s polling, now matches server capacity)

// Color palette for different sensors
const colors = [
  '#3498db', '#e74c3c', '#2ecc71', '#f39c12', '#9b59b6', 
  '#1abc9c', '#e67e22', '#34495e', '#f1c40f', '#e91e63'
];

// === CHART MODE OPTIONS FUNCTIONALITY ===
function updateChartModeOptions() {
  if (!chartModeSel) {
    console.log('chartModeSel not available for updateChartModeOptions');
    return;
  }
  
  // Store the current selection
  const currentSelection = chartModeSel.value;
  
  // Clear existing options except "All Sensors"
  chartModeSel.innerHTML = '<option value="all">All Sensors</option>';
  
  // Add options for each detected sensor
  const sensors = [];
  for (const [mac, hist] of historyByMac.entries()) {
    // Get the display name from the sensor card
    const cardId = "card-" + mac.replace(/:/g, "");
    const card = document.getElementById(cardId);
    const displayName = card ? card.querySelector('.sensor-name').textContent : mac;
    
    sensors.push({ mac, displayName });
  }
  
  if (sensors.length === 0) {
    return; // No individual sensor options to add
  }
  
  // Sort sensors by display name for better UX
  sensors.sort((a, b) => a.displayName.localeCompare(b.displayName));
  
  // Add individual sensor options
  sensors.forEach((sensor) => {
    const option = document.createElement('option');
    option.value = sensor.mac;
    option.textContent = sensor.displayName;
    chartModeSel.appendChild(option);
  });
  
  // Restore selection if it's still valid, otherwise default to "All Sensors"
  if (currentSelection && chartModeSel.querySelector(`option[value="${currentSelection}"]`)) {
    chartModeSel.value = currentSelection;
  } else {
    chartModeSel.value = "all";
  }
}

// === CLOCK FUNCTIONALITY ===
function updateClock() {
  const now = new Date();
  
  // Format time (HH:MM:SS)
  const timeStr = now.toLocaleTimeString('en-US', {
    hour12: false,
    hour: '2-digit',
    minute: '2-digit',
    second: '2-digit'
  });
  
  // Format date (Day, Month DD, YYYY)
  const dateStr = now.toLocaleDateString('en-US', {
    weekday: 'long',
    year: 'numeric',
    month: 'long',
    day: 'numeric'
  });
  
  // Get timezone
  const timezoneStr = Intl.DateTimeFormat().resolvedOptions().timeZone;
  
  // Update DOM elements
  if (currentTimeEl) currentTimeEl.textContent = timeStr;
  if (currentDateEl) currentDateEl.textContent = dateStr;
  if (timezoneEl) timezoneEl.textContent = timezoneStr;
  
  // Update greeting and background
  updateGreetingAndBackground(now);
}

function updateGreetingAndBackground(now) {
  const hour = now.getHours();
  const greetingEl = document.getElementById('clockGreeting');
  const backgroundEl = document.getElementById('clockBackground');
  
  if (!greetingEl || !backgroundEl) return;
  
  let greeting, icon, backgroundSvg;
  
  if (hour >= 5 && hour < 12) {
    // Morning (5 AM - 11:59 AM)
    greeting = 'Good Morning';
    icon = '🌅';
    backgroundSvg = createSunriseBackground();
  } else if (hour >= 12 && hour < 17) {
    // Afternoon (12 PM - 4:59 PM)
    greeting = 'Good Afternoon';
    icon = '☀️';
    backgroundSvg = createSunBackground();
  } else if (hour >= 17 && hour < 21) {
    // Evening (5 PM - 8:59 PM)
    greeting = 'Good Evening';
    icon = '🌅';
    backgroundSvg = createSunsetBackground();
  } else {
    // Night (9 PM - 4:59 AM)
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
      <!-- Sky gradient -->
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
      <!-- Sun -->
      <circle cx="200" cy="60" r="25" fill="url(#sun)" opacity="0.9"/>
      <!-- Sun rays -->
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
      <!-- Mountains -->
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
      <!-- Sun -->
      <circle cx="200" cy="40" r="30" fill="url(#sunDay)" opacity="0.9"/>
      <!-- Sun rays -->
      <g stroke="#ffff00" stroke-width="3" opacity="0.7">
        <line x1="200" y1="0" x2="200" y2="5"/>
        <line x1="240" y1="20" x2="250" y2="15"/>
        <line x1="260" y1="40" x2="270" y2="40"/>
        <line x1="240" y1="60" x2="250" y2="65"/>
        <line x1="200" y1="80" x2="200" y2="85"/>
        <line x1="160" y1="60" x2="150" y2="65"/>
        <line x1="140" y1="40" x2="130" y2="40"/>
        <line x1="160" y1="20" x2="150" y2="15"/>
      </g>
      <!-- Clouds -->
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
      <!-- Sun -->
      <circle cx="200" cy="80" r="25" fill="url(#sunsetSun)" opacity="0.9"/>
      <!-- Sun rays -->
      <g stroke="#ffcc80" stroke-width="2" opacity="0.6">
        <line x1="200" y1="30" x2="200" y2="20"/>
        <line x1="230" y1="50" x2="240" y2="40"/>
        <line x1="240" y1="80" x2="250" y2="80"/>
        <line x1="230" y1="110" x2="240" y2="120"/>
        <line x1="200" y1="130" x2="200" y2="140"/>
        <line x1="170" y1="110" x2="160" y2="120"/>
        <line x1="160" y1="80" x2="150" y2="80"/>
        <line x1="170" y1="50" x2="160" y2="40"/>
      </g>
      <!-- Mountains -->
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
      <!-- Moon -->
      <circle cx="200" cy="50" r="20" fill="url(#moon)" opacity="0.9"/>
      <!-- Stars -->
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
      <!-- Mountains -->
      <polygon points="0,200 50,150 100,180 150,120 200,160 250,100 300,140 350,110 400,150 400,200" fill="#1a1a1a" opacity="0.9"/>
    </svg>
  `;
}

// === ESP32 SYSTEM INFO FUNCTIONALITY ===
async function updateESP32Info() {
  console.log('Attempting to fetch ESP32 health info...');
  try {
    const response = await safeFetch('/health', { 
      cache: 'no-store',
      headers: {
        'Accept': 'application/json',
        'Cache-Control': 'no-cache',
        'Pragma': 'no-cache'
      }
    });
    console.log('Health response status:', response.status, response.statusText);
    if (!response.ok) {
      console.log('Failed to fetch ESP32 health info - HTTP', response.status);
      return;
    }
    
    const data = await response.json();
    
    // Update domain name
    if (esp32DomainEl) {
      const hostname = window.location.hostname;
      const port = window.location.port ? `:${window.location.port}` : '';
      
      if (hostname.includes('.local') || hostname.includes('.lan')) {
        esp32DomainEl.textContent = `${hostname}${port}`;
      } else if (hostname.match(/^\d+\.\d+\.\d+\.\d+$/)) {
        esp32DomainEl.textContent = 'Direct IP';
      } else {
        esp32DomainEl.textContent = `${hostname}${port}`;
      }
    }
    
    // Update IP address (show only the real IP)
    if (esp32IPEl) {
      if (data.ip) {
        esp32IPEl.textContent = data.ip;
      } else {
        const hostname = window.location.hostname;
        if (hostname.match(/^\d+\.\d+\.\d+\.\d+$/)) {
          esp32IPEl.textContent = hostname;
        } else {
          esp32IPEl.textContent = 'Unknown';
        }
      }
    }
    
    // Update heap memory
    if (esp32HeapEl && data.heap) {
      const heapKB = Math.round(data.heap / 1024);
      esp32HeapEl.textContent = `${heapKB} KB`;
    }
    
    // Update uptime
    if (esp32UptimeEl && data.uptime_ms) {
      const uptimeSeconds = Math.floor(data.uptime_ms / 1000);
      const hours = Math.floor(uptimeSeconds / 3600);
      const minutes = Math.floor((uptimeSeconds % 3600) / 60);
      const seconds = uptimeSeconds % 60;
      esp32UptimeEl.textContent = `${hours}h ${minutes}m ${seconds}s`;
    }
    
    // Update WiFi mode
    if (esp32WifiModeEl && data.ap_mode !== undefined) {
      esp32WifiModeEl.textContent = data.ap_mode ? 'AP Mode' : 'STA Mode';
    }
    
    // Update HTTP requests
    if (esp32HttpReqsEl && data.req_total !== undefined) {
      esp32HttpReqsEl.textContent = data.req_total.toString();
    }
    
    // Update BLE adverts
    if (esp32BleAdvertsEl && data.ble_seen !== undefined) {
      esp32BleAdvertsEl.textContent = data.ble_seen.toString();
    }
    
    // Update CPU usage
    if (esp32CpuUsageEl && data.cpu_usage !== undefined) {
      esp32CpuUsageEl.textContent = `${data.cpu_usage.toFixed(1)}%`;
    }
    
    // Update circuit background
    updateESP32Background(data);
    
  } catch (error) {
    console.log('Error fetching ESP32 info:', error);
  }
}

function updateESP32Background(data) {
  const backgroundEl = document.getElementById('esp32Background');
  if (!backgroundEl) return;
  
  // Create circuit illustration based on system status
  const circuitSvg = createCircuitBackground(data);
  backgroundEl.innerHTML = circuitSvg;
}

function initializeESP32Background() {
  const backgroundEl = document.getElementById('esp32Background');
  if (!backgroundEl) return;
  
  // Set initial background even without data
  const initialData = {
    heap: 25000,
    uptime_ms: 1000,
    ap_mode: false
  };
  const circuitSvg = createCircuitBackground(initialData);
  backgroundEl.innerHTML = circuitSvg;
}

function createCircuitBackground(data) {
  // Determine circuit state based on system health
  const isHealthy = data.heap > 20000 && data.uptime_ms > 0;
  const isWifiConnected = !data.ap_mode;
  
  return `
    <svg viewBox="0 0 400 200" xmlns="http://www.w3.org/2000/svg">
      <defs>
        <linearGradient id="circuitBg" x1="0%" y1="0%" x2="100%" y2="100%">
          <stop offset="0%" style="stop-color:#0ea5e9;stop-opacity:0.1" />
          <stop offset="100%" style="stop-color:#0284c7;stop-opacity:0.2" />
        </linearGradient>
        <filter id="glow">
          <feGaussianBlur stdDeviation="2" result="coloredBlur"/>
          <feMerge> 
            <feMergeNode in="coloredBlur"/>
            <feMergeNode in="SourceGraphic"/>
          </feMerge>
        </filter>
      </defs>
      
      <!-- Background -->
      <rect width="400" height="200" fill="url(#circuitBg)"/>
      
      <!-- Main ESP32 Chip -->
      <rect x="150" y="80" width="100" height="60" rx="8" fill="rgba(255,255,255,0.2)" stroke="rgba(255,255,255,0.4)" stroke-width="2"/>
      <text x="200" y="105" text-anchor="middle" fill="rgba(255,255,255,0.8)" font-size="12" font-family="monospace">ESP32</text>
      
      <!-- Circuit traces -->
      <g stroke="rgba(255,255,255,0.6)" stroke-width="2" fill="none">
        <!-- Power lines -->
        <line x1="50" y1="50" x2="150" y2="50"/>
        <line x1="250" y1="50" x2="350" y2="50"/>
        <line x1="50" y1="150" x2="150" y2="150"/>
        <line x1="250" y1="150" x2="350" y2="150"/>
        
        <!-- Data lines -->
        <line x1="50" y1="100" x2="150" y2="100"/>
        <line x1="250" y1="100" x2="350" y2="100"/>
        <line x1="200" y1="50" x2="200" y2="80"/>
        <line x1="200" y1="140" x2="200" y2="150"/>
      </g>
      
      <!-- Components -->
      <!-- Resistors -->
      <rect x="80" y="45" width="20" height="10" fill="rgba(255,255,255,0.3)" rx="2"/>
      <rect x="300" y="45" width="20" height="10" fill="rgba(255,255,255,0.3)" rx="2"/>
      
      <!-- Capacitors -->
      <line x1="80" y1="95" x2="80" y2="105"/>
      <line x1="85" y1="95" x2="85" y2="105"/>
      <line x1="300" y1="95" x2="300" y2="105"/>
      <line x1="305" y1="95" x2="305" y2="105"/>
      
      <!-- LEDs (status indicators) -->
      <circle cx="100" cy="50" r="3" fill="${isHealthy ? '#10b981' : '#ef4444'}" filter="url(#glow)"/>
      <circle cx="300" cy="50" r="3" fill="${isWifiConnected ? '#10b981' : '#f59e0b'}" filter="url(#glow)"/>
      <circle cx="100" cy="150" r="3" fill="#6366f1" filter="url(#glow)"/>
      <circle cx="300" cy="150" r="3" fill="#8b5cf6" filter="url(#glow)"/>
      
      <!-- WiFi antenna -->
      <path d="M 200 20 Q 180 40 200 60 Q 220 40 200 20" stroke="rgba(255,255,255,0.6)" stroke-width="2" fill="none"/>
      
      <!-- Bluetooth symbol -->
      <path d="M 200 160 Q 185 175 200 190 Q 215 175 200 160" stroke="rgba(255,255,255,0.6)" stroke-width="2" fill="none"/>
      
      <!-- Data flow indicators -->
      <g opacity="0.7">
        <circle cx="120" cy="50" r="2" fill="rgba(255,255,255,0.8)">
          <animate attributeName="opacity" values="0.3;1;0.3" dur="2s" repeatCount="indefinite"/>
        </circle>
        <circle cx="280" cy="50" r="2" fill="rgba(255,255,255,0.8)">
          <animate attributeName="opacity" values="0.3;1;0.3" dur="2.5s" repeatCount="indefinite"/>
        </circle>
        <circle cx="120" cy="150" r="2" fill="rgba(255,255,255,0.8)">
          <animate attributeName="opacity" values="0.3;1;0.3" dur="1.8s" repeatCount="indefinite"/>
        </circle>
        <circle cx="280" cy="150" r="2" fill="rgba(255,255,255,0.8)">
          <animate attributeName="opacity" values="0.3;1;0.3" dur="2.2s" repeatCount="indefinite"/>
        </circle>
      </g>
    </svg>
  `;
}

// Start clock updates
let clockInterval = setInterval(updateClock, 1000);
updateClock(); // Initial update

// === SENSOR CARDS ===
function createSensorCard(tag) {
  const displayName = tag.name && tag.name.length ? tag.name : tag.mac;
  const isOnline = tag.age < 30; // Consider online if seen within 30 seconds
  
  return `
    <div class="sensor-card" id="card-${tag.mac.replace(/:/g, '')}">
      <div class="sensor-card-header">
        <div class="sensor-name">${displayName}</div>
        <div class="sensor-status ${isOnline ? '' : 'offline'}"></div>
      </div>
      <div class="sensor-metrics">
        <div class="metric">
          <span class="metric-label">🌡️ Temp:</span>
          <span class="metric-value">${tag.t != null ? Number(tag.t).toFixed(1) + '°C' : 'N/A'}</span>
        </div>
        <div class="metric">
          <span class="metric-label">💧 Humidity:</span>
          <span class="metric-value">${tag.h != null ? Number(tag.h).toFixed(1) + '%' : 'N/A'}</span>
        </div>
        <div class="metric">
          <span class="metric-label">🔽 Pressure:</span>
          <span class="metric-value">${tag.p != null ? Number(tag.p).toFixed(0) + ' Pa' : 'N/A'}</span>
        </div>
        <div class="metric">
          <span class="metric-label">🔋 Battery:</span>
          <span class="metric-value">${tag.batt != null ? tag.batt + ' mV' : 'N/A'}</span>
        </div>
        <div class="metric">
          <span class="metric-label">📶 Signal:</span>
          <span class="metric-value">${tag.rssi} dBm</span>
        </div>
        <div class="metric">
          <span class="metric-label">⏱️ Last Seen:</span>
          <span class="metric-value">${tag.age}s ago</span>
        </div>
      </div>
    </div>
  `;
}

function updateSensorCard(tag) {
  const cardId = `card-${tag.mac.replace(/:/g, '')}`;
  let card = document.getElementById(cardId);
  
  if (!card) {
    // Create new card
    sensorCards.insertAdjacentHTML('beforeend', createSensorCard(tag));
  } else {
    // Update existing card
    card.outerHTML = createSensorCard(tag);
  }
}


// === CHART HANDLING ===
function makeChart(ctx) {
  // Destroy existing chart if it exists
  if (typeof Chart !== 'undefined' && Chart.getChart(ctx.canvas)) {
    Chart.getChart(ctx.canvas).destroy();
  }
  
  return new Chart(ctx, {
    type: 'line',
    data: { datasets: [] },
    options: {
      // iPad Safari optimizations
      animation: false, // Disable animations for iPad Safari
      responsive: true,
      maintainAspectRatio: false,
      resizeDelay: 200,
      // Force pixel ratio for iPad Safari
      devicePixelRatio: window.devicePixelRatio || 1,
      scales: {
        x: {
          type: 'linear', // Use linear instead of time to avoid date adapter issues
          title: { display: true, text: 'Time (minutes ago)' },
          reverse: true, // Reverse axis so "now" (0) is on the right, past on the left
          ticks: { 
            autoSkip: true, 
            maxTicksLimit: 8,
            callback: function(value) {
              // Convert to minutes ago format
              const minutes = Math.round(value);
              return minutes === 0 ? 'Now' : `${minutes}m ago`;
            }
          }
        },
        y: {
          title: { display: true, text: getChartTitle() },
          ticks: { maxTicksLimit: 6 }
        }
      },
      plugins: {
        legend: { 
          display: true,
          position: 'top',
          labels: { usePointStyle: true, padding: 20 }
        },
        tooltip: { 
          mode: 'nearest', 
          intersect: false,
          callbacks: {
            title: function(context) {
              const minutes = Math.round(context[0].parsed.x);
              return minutes === 0 ? 'Now' : `${minutes} minutes ago`;
            },
            label: function(context) {
              const label = context.dataset.label || '';
              const value = context.parsed.y;
              return `${label}: ${value}`;
            }
          }
        }
      }
    }
  });
}

function getChartTitle() {
  const titles = {
    'temperature': 'Temperature (°C)',
    'humidity': 'Humidity (%)',
    'pressure': 'Pressure (Pa)'
    // Removed battery and RSSI chart options to save space
  };
  return titles[currentChartType] || 'Chart';
}

function getDataKey() {
  const keys = {
    'temperature': 't',
    'humidity': 'h',
    'pressure': 'p'
    // Removed battery and RSSI data keys to save space
  };
  return keys[currentChartType] || 't';
}

// Initialize chart with null check and delayed initialization for iPad
let mainChart = null;

function initializeChart() {
  try {
    const chartCanvas = document.getElementById('mainChart');
    if (!chartCanvas) {
      console.log('Chart canvas not found, will retry later');
      return false;
    }
    
    if (typeof Chart === 'undefined') {
      console.log('Chart.js not loaded yet, will retry later');
      return false;
    }
    
    // Destroy existing chart if it exists
    if (mainChart) {
      console.log('Destroying existing chart');
      mainChart.destroy();
      mainChart = null;
    }
    
    // Also check for any chart registered with the canvas
    const existingChart = Chart.getChart(chartCanvas);
    if (existingChart) {
      console.log('Destroying existing chart from canvas');
      existingChart.destroy();
    }
    
    // Try makeChart first if available
    if (typeof makeChart === 'function') {
      console.log('Using makeChart function');
      mainChart = makeChart(chartCanvas.getContext('2d'));
    } else {
      console.log('makeChart not available, creating simple chart');
      // Create a simple chart directly without time scale
      mainChart = new Chart(chartCanvas.getContext('2d'), {
        type: 'line',
        data: {
          labels: [],
          datasets: []
        },
        options: {
          responsive: true,
          maintainAspectRatio: false,
          animation: false,
          scales: {
            x: {
              type: 'linear',
              title: { display: true, text: 'Time (minutes ago)' },
              reverse: true, // Reverse axis so "now" (0) is on the right, past on the left
              ticks: {
                callback: function(value) {
                  const minutes = Math.round(value);
                  return minutes === 0 ? 'Now' : `${minutes}m ago`;
                }
              }
            },
            y: {
              title: {
                display: true,
                text: 'Temperature (°C)'
              }
            }
          }
        }
      });
    }
    
    console.log('Chart initialized successfully');
    return true;
  } catch (error) {
    console.error('Failed to initialize chart:', error);
    // Clean up on error
    if (mainChart) {
      try {
        mainChart.destroy();
      } catch (e) {
        console.log('Error destroying chart:', e);
      }
      mainChart = null;
    }
    return false;
  }
}

// Clean up chart on page unload
window.addEventListener('beforeunload', function() {
  if (mainChart) {
    try {
      mainChart.destroy();
      mainChart = null;
    } catch (e) {
      console.log('Error destroying chart on unload:', e);
    }
  }
});

// Initialize chart with iPad Safari timing
window.addEventListener('load', function() {
  // Wait for layout to settle on iPad Safari
  setTimeout(function() {
    if (!initializeChart()) {
      // If immediate initialization fails, try again after a delay
      setTimeout(() => {
        if (!initializeChart()) {
          // Final attempt after another delay
          setTimeout(initializeChart, 1000);
        }
      }, 500);
    }
  }, 100); // Small delay for iPad Safari layout settling
});

function ensureHistory(mac, displayName) {
  if (!historyByMac.has(mac)) {
    historyByMac.set(mac, { t: [], h: [], p: [] }); // Removed batt and rssi to save space
  }
}

// Deduplication function to remove duplicate timestamps
function deduplicateDataPoints(dataArray) {
  if (!dataArray || dataArray.length === 0) return dataArray;
  
  // Sort by timestamp first
  dataArray.sort((a, b) => a.x - b.x);
  
  // Remove duplicates (same timestamp within 1 second tolerance)
  const deduplicated = [];
  let lastTimestamp = 0;
  
  for (const point of dataArray) {
    if (Math.abs(point.x - lastTimestamp) > 1000) { // 1 second tolerance
      deduplicated.push(point);
      lastTimestamp = point.x;
    }
  }
  
  return deduplicated;
}

function trimHistory(arr, windowMs) {
  const cutoff = Date.now() - windowMs;
  while (arr.length && arr[0].x < cutoff) arr.shift();
  if (arr.length > MAX_POINTS) arr.splice(0, arr.length - MAX_POINTS);
}

function renderChart() {
  if (!mainChart) {
    console.log('mainChart not available for renderChart, attempting to initialize...');
    initializeChart();
    if (!mainChart) {
      console.log('Failed to initialize chart');
      return;
    }
  }
  
  const dataKey = getDataKey();
  const datasets = [];
  
  // Calculate cutoff time for the selected time window
  let cutoff = Date.now() - windowMs;
  
  // Find the earliest available data point for debugging
  let earliestTime = Date.now();
  if (historyByMac.size > 0) {
    for (const [mac, hist] of historyByMac.entries()) {
      if (hist[dataKey] && hist[dataKey].length > 0) {
        const earliestPoint = Math.min(...hist[dataKey].map(point => point.x));
        earliestTime = Math.min(earliestTime, earliestPoint);
      }
    }
  }
  
  // Apply the user's selected time window
  // If the requested window is longer than available data, show all available data
  const availableDataStart = earliestTime;
  const requestedWindowStart = cutoff;
  const actualWindowStart = Math.max(availableDataStart, requestedWindowStart);
  
  // If the requested window is longer than available data, use the earliest data point
  const effectiveWindowStart = (windowMs > (Date.now() - earliestTime)) ? earliestTime : actualWindowStart;
  
  console.log(`Time window: ${windowMs/1000/60} minutes, cutoff: ${new Date(cutoff).toISOString()}, earliest data: ${new Date(earliestTime).toISOString()}`);
  console.log(`Actual window start: ${new Date(actualWindowStart).toISOString()}`);
  console.log(`Effective window start: ${new Date(effectiveWindowStart).toISOString()}`);
  
  // Inform user about data availability
  const availableDataMinutes = (Date.now() - earliestTime) / 1000 / 60;
  if (windowMs > availableDataMinutes * 60 * 1000) {
    console.log(`Note: Requested ${windowMs/1000/60} minutes, but only ${availableDataMinutes.toFixed(1)} minutes of historical data available. Showing all available data.`);
  }
  
  if (currentChartMode === 'all') {
    // Show all sensors on one chart
    let colorIndex = 0;
    for (const [mac, hist] of historyByMac.entries()) {
      // Get the display name from the sensor card
      const cardId = "card-" + mac.replace(/:/g, "");
      const card = document.getElementById(cardId);
      const displayName = card ? card.querySelector('.sensor-name').textContent : mac;
      
      // Filter data based on the selected time window
      const allData = hist[dataKey] || [];
      const filteredData = allData.filter(point => point.x >= effectiveWindowStart);
      
      // Validate data points
      const validData = filteredData.filter(point => 
        point.x && point.y !== null && point.y !== undefined && !isNaN(point.y)
      );
      
      // Apply final deduplication to ensure clean chart rendering
      const cleanData = deduplicateDataPoints(validData);
      
      // Convert timestamps to "minutes ago" for linear scale
      const now = Date.now();
      const convertedData = cleanData.map(point => ({
        x: (now - point.x) / (1000 * 60), // Convert to minutes ago
        y: point.y
      }));
      
      console.log(`Data filtering for ${mac}: ${allData.length} total points, ${filteredData.length} after time window filtering, ${validData.length} valid points, ${cleanData.length} clean points`);
      
      if (cleanData.length > 0) {
        const timeSpan = (cleanData[cleanData.length-1].x - cleanData[0].x) / 1000 / 60; // minutes
        console.log(`Time span: ${timeSpan.toFixed(1)} minutes - First: ${new Date(cleanData[0].x).toISOString()}, Last: ${new Date(cleanData[cleanData.length-1].x).toISOString()}`);
      }
      
      datasets.push({
        label: displayName,
        data: convertedData, // Use converted data for linear scale
        fill: false,
        tension: 0.15,
        pointRadius: 0,
        borderColor: colors[colorIndex % colors.length],
        backgroundColor: colors[colorIndex % colors.length] + '20'
      });
      colorIndex++;
    }
  } else {
    // Show individual sensor (selected from chart mode dropdown)
    const selectedMac = currentChartMode; // The selected option value is the MAC address
    if (!selectedMac || selectedMac === 'all') {
      console.log('No individual sensor selected');
      return;
    }
    if (historyByMac.has(selectedMac)) {
      const hist = historyByMac.get(selectedMac);
      const cardId = "card-" + selectedMac.replace(/:/g, "");
      const card = document.getElementById(cardId);
      const displayName = card ? card.querySelector('.sensor-name').textContent : selectedMac;
      
      // Filter data based on actual available data window
      const allData = hist[dataKey] || [];
      const filteredData = allData.filter(point => point.x >= actualWindowStart);
      
      // Validate data points
      const validData = filteredData.filter(point => 
        point.x && point.y !== null && point.y !== undefined && !isNaN(point.y)
      );
      
      // Apply final deduplication to ensure clean chart rendering
      const cleanData = deduplicateDataPoints(validData);
      
      // Convert timestamps to "minutes ago" for linear scale
      const now = Date.now();
      const convertedData = cleanData.map(point => ({
        x: (now - point.x) / (1000 * 60), // Convert to minutes ago
        y: point.y
      }));
      
      console.log(`Data filtering for ${selectedMac} (individual mode): ${allData.length} total points, ${filteredData.length} after time window filtering, ${validData.length} valid points, ${cleanData.length} clean points`);
      
      if (cleanData.length > 0) {
        const timeSpan = (cleanData[cleanData.length-1].x - cleanData[0].x) / 1000 / 60; // minutes
        console.log(`Time span: ${timeSpan.toFixed(1)} minutes - First: ${new Date(cleanData[0].x).toISOString()}, Last: ${new Date(cleanData[cleanData.length-1].x).toISOString()}`);
      }
      
      datasets.push({
        label: displayName,
        data: convertedData, // Use converted data for linear scale
        fill: false,
        tension: 0.15,
        pointRadius: 0,
        borderColor: colors[0],
        backgroundColor: colors[0] + '20'
      });
    } else {
      console.log(`Selected sensor ${selectedMac} not found in history`);
    }
  }
  
  mainChart.data.datasets = datasets;
  
  // Update chart title safely
  if (typeof getChartTitle === 'function') {
    mainChart.options.scales.y.title.text = getChartTitle();
  } else if (mainChart.options && mainChart.options.scales && mainChart.options.scales.y && mainChart.options.scales.y.title) {
    mainChart.options.scales.y.title.text = 'Chart';
  }
  
  // Let Chart.js auto-scale the x-axis based on the data
  // The linear scale with "minutes ago" format will automatically adjust
  console.log(`Chart updated with ${datasets.length} datasets`);
  if (datasets.length > 0 && datasets[0].data.length > 0) {
    console.log(`First dataset has ${datasets[0].data.length} data points`);
    console.log(`X-axis range: ${datasets[0].data[0].x} to ${datasets[0].data[datasets[0].data.length-1].x} minutes ago`);
  }
  
  mainChart.update();
}

// Event listeners - with null checks
if (chartTypeSel) {
  chartTypeSel.addEventListener('change', () => {
    currentChartType = chartTypeSel.value;
    if (chartTitle) {
      const title = typeof getChartTitle === 'function' ? getChartTitle() : 'Chart';
      chartTitle.textContent = `📈 ${title}`;
    }
    renderChart();
  });
}

if (chartModeSel) {
  chartModeSel.addEventListener('change', () => {
    currentChartMode = chartModeSel.value;
    renderChart();
  });
}

if (windowSel) {
  windowSel.addEventListener('change', async () => {
    windowMs = Number(windowSel.value);
    console.log(`Time window changed to: ${windowMs/1000/60} minutes`);
    
    // Reload history from server to get fresh data for the new time window
    try {
      console.log('Reloading history data for new time window...');
      
      // Show loading indicator
      const chartContainer = document.querySelector('.chart-container');
      if (chartContainer) {
        chartContainer.style.opacity = '0.6';
        chartContainer.style.pointerEvents = 'none';
      }
      
      await loadHistoryFromServer();
      console.log('History reloaded successfully');
      
      // Hide loading indicator
      if (chartContainer) {
        chartContainer.style.opacity = '1';
        chartContainer.style.pointerEvents = 'auto';
      }
    } catch (error) {
      console.error('Failed to reload history for new time window:', error);
      
      // Hide loading indicator even on error
      const chartContainer = document.querySelector('.chart-container');
      if (chartContainer) {
        chartContainer.style.opacity = '1';
        chartContainer.style.pointerEvents = 'auto';
      }
      // Continue with existing data if reload fails
    }
    
    // Re-render the chart with the fresh data and new time window
    renderChart();
  });
}

// Chart mode now handles individual sensor selection directly

// === POLLING CONTROL ===
let inFlight = false;
let pollIntervalMs = 8000; // normal poll rate (compromise between 5s and 10s)
let timerId = null;

// Fallback fetch for older Safari versions
function safeFetch(url, options = {}) {
  return new Promise((resolve, reject) => {
    // Try modern fetch first
    if (typeof fetch !== 'undefined') {
      fetch(url, options)
        .then(resolve)
        .catch(reject);
    } else {
      // Fallback to XMLHttpRequest
      console.log('Using XMLHttpRequest fallback for', url);
      const xhr = new XMLHttpRequest();
      xhr.open(options.method || 'GET', url, true);
      
      // Set headers
      if (options.headers) {
        for (const [key, value] of Object.entries(options.headers)) {
          xhr.setRequestHeader(key, value);
        }
      }
      
      xhr.onreadystatechange = function() {
        if (xhr.readyState === 4) {
          if (xhr.status >= 200 && xhr.status < 300) {
            resolve({
              ok: true,
              status: xhr.status,
              statusText: xhr.statusText,
              json: () => Promise.resolve(JSON.parse(xhr.responseText)),
              text: () => Promise.resolve(xhr.responseText)
            });
          } else {
            reject(new Error('HTTP ' + xhr.status));
          }
        }
      };
      
      xhr.onerror = () => reject(new Error('Network error'));
      xhr.send();
    }
  });
}

async function tick() {
  if (inFlight) return; // prevent overlaps
  inFlight = true;
  console.log('Attempting to fetch sensor data...');
  try {
    const r = await safeFetch('/data', { 
      cache: 'no-store',
      headers: {
        'Accept': 'application/json',
        'Cache-Control': 'no-cache',
        'Pragma': 'no-cache'
      }
    });
    console.log('Data response status:', r.status, r.statusText);
    if (!r.ok) {
      console.log('Failed to fetch data - HTTP', r.status);
      throw new Error('HTTP ' + r.status);
    }

    const snap = await r.json();
    console.log('Received data:', snap);
    if (!snap || typeof snap !== 'object') {
      console.log('Invalid JSON response:', snap);
      throw new Error('Invalid JSON response from server');
    }
    const now = Date.now();

    if (snap && Array.isArray(snap.tags)) {
      snap.tags.forEach(tag => {
        // Update sensor card (top cards)
        updateSensorCard(tag);
        
        // Update history for charts
        const displayName = tag.name && tag.name.length ? tag.name : tag.mac;
        ensureHistory(tag.mac, displayName);
        const hist = historyByMac.get(tag.mac);
        
        // Store environmental data only (battery and RSSI not stored in history to save space)
        // Check for duplicates before adding new data points
        if (tag.t != null) {
          const newPoint = { x: now, y: Number(tag.t) };
          // Remove any existing point with the same timestamp (within 1 second tolerance)
          hist.t = hist.t.filter(point => Math.abs(point.x - now) > 1000);
          hist.t.push(newPoint);
        }
        if (tag.h != null) {
          const newPoint = { x: now, y: Number(tag.h) };
          hist.h = hist.h.filter(point => Math.abs(point.x - now) > 1000);
          hist.h.push(newPoint);
        }
        if (tag.p != null) {
          const newPoint = { x: now, y: Number(tag.p) };
          hist.p = hist.p.filter(point => Math.abs(point.x - now) > 1000);
          hist.p.push(newPoint);
        }
        
        // Ensure data is sorted by timestamp for proper chart rendering
        hist.t.sort((a, b) => a.x - b.x);
        hist.h.sort((a, b) => a.x - b.x);
        hist.p.sort((a, b) => a.x - b.x);
        
        // Trim history to respect the selected time window and prevent memory bloat
        trimHistory(hist.t, windowMs);
        trimHistory(hist.h, windowMs);
        trimHistory(hist.p, windowMs);
      });

      // Update chart mode options with new sensors
      updateChartModeOptions();
      
      // Update the main chart
      renderChart();
    }

    // Reset backoff after a successful poll
    if (pollIntervalMs !== 10000) {
      pollIntervalMs = 10000;
      restartTimer();
    }
  } catch (e) {
    console.log('Poll error details:', e);
    console.log('Error type:', typeof e);
    console.log('Error message:', e.message);
    console.log('Error stack:', e.stack);
    
    // Show error in debug panel if available
    if (window.debugPanel) {
      window.debugPanel.innerHTML += `<div style="color: red;">Error: ${e.message}</div>`;
    }
    
    // Exponential backoff on error (max 60 seconds)
    pollIntervalMs = Math.min(pollIntervalMs * 1.5, 60000);
    console.log('Retrying in', pollIntervalMs, 'ms');
    restartTimer();
  } finally {
    inFlight = false;
  }
}

function restartTimer() {
  if (timerId) clearInterval(timerId);
  timerId = setInterval(tick, pollIntervalMs);
}

// Pause/resume polling when the tab is hidden/visible
document.addEventListener('visibilitychange', () => {
  if (document.hidden) {
    if (timerId) clearInterval(timerId), timerId = null;
    if (clockInterval) clearInterval(clockInterval), clockInterval = null;
  } else {
    restartTimer();
    tick();
    // Restart clock updates
    if (!clockInterval) {
      clockInterval = setInterval(updateClock, 1000);
      updateClock(); // Immediate update
    }
  }
});

// iPad/Safari touch event handling
document.addEventListener('touchstart', function(e) {
  // Prevent zoom on double-tap for better UX
  if (e.touches.length > 1) {
    e.preventDefault();
  }
}, { passive: false });

// Add touch-friendly hover effects for iPad
if ('ontouchstart' in window) {
  document.body.classList.add('touch-device');
}

// === SERVER HISTORY LOADING ===

// Parse binary history data
function parseBinaryHistory(arrayBuffer) {
  const view = new DataView(arrayBuffer);
  let offset = 0;
  
  // Read header
  const magic = view.getUint32(offset, true); // Little-endian (ESP32 format)
  offset += 4;
  if (magic !== 0x48495354) { // "HIST"
    throw new Error('Invalid binary format magic');
  }
  
  const version = view.getUint16(offset, true); // Little-endian
  offset += 2;
  const sensorCount = view.getUint16(offset, true); // Little-endian
  offset += 2;
  const serverTime = view.getUint32(offset, true); // Little-endian
  offset += 4;
  
  const result = { serverTime, data: {} };
  
  // Read sensor data
  for (let i = 0; i < sensorCount; i++) {
    // Check bounds before reading sensor header (20 bytes total)
    if (offset + 20 > arrayBuffer.byteLength) {
      console.warn(`Sensor ${i} header would exceed buffer bounds`);
      break;
    }
    
    // Read sensor header - MAC address is 18 bytes (including null terminator)
    const macBytes = new Uint8Array(arrayBuffer.slice(offset, offset + 18));
    const mac = new TextDecoder().decode(macBytes).replace(/\0/g, '');
    offset += 18;
    
    const pointCount = view.getUint16(offset, true); // Little-endian
    offset += 2;
    
    // Calculate maximum possible points based on remaining buffer space
    const remainingBytes = arrayBuffer.byteLength - offset;
    const maxPossiblePoints = Math.floor(remainingBytes / 12); // 12 bytes per data point
    const actualPointCount = Math.min(pointCount, maxPossiblePoints);
    
    if (pointCount > maxPossiblePoints) {
      console.warn(`Sensor ${i}: Reported ${pointCount} points but only ${maxPossiblePoints} can fit in buffer`);
    }
    
    const sensorData = { t: [], h: [], p: [] };
    
    // Read data points
    for (let j = 0; j < actualPointCount; j++) {
      // Check bounds before reading each data point (12 bytes total)
      if (offset + 12 > arrayBuffer.byteLength) {
        console.warn(`Data point ${j} would exceed buffer bounds, stopping at ${j}/${actualPointCount} points`);
        break;
      }
      
      const timestamp = view.getUint32(offset, true); // ESP32 millis() - milliseconds since boot (little-endian)
      offset += 4;
      
      const temp = view.getUint16(offset, true); // Little-endian
      offset += 2;
      const humidity = view.getUint16(offset, true); // Little-endian
      offset += 2;
      const pressure = view.getUint32(offset, true); // Little-endian
      offset += 4;
      
      // Convert back to original values with data validation
      // Temperature: stored as (temp * 100) as int16, so divide by 100
      if (temp !== 0xFFFF && temp < 10000) { // Valid temp range: -100°C to +100°C
        sensorData.t.push({ x: timestamp, y: temp / 100 });
      }
      // Humidity: stored as (humidity * 100) as int16, so divide by 100
      if (humidity !== 0xFFFF && humidity <= 10000) { // Valid humidity range: 0% to 100%
        sensorData.h.push({ x: timestamp, y: humidity / 100 });
      }
      // Pressure: stored as Pa (Pascal), convert to kPa
      // Normal atmospheric pressure is ~101,325 Pa (101.325 kPa)
      if (pressure !== 0xFFFFFFFF && pressure > 80000 && pressure < 120000) { // Valid pressure range: 80-120 kPa
        sensorData.p.push({ x: timestamp, y: pressure / 1000 }); // Convert Pa to kPa
      }
    }
    
    result.data[mac] = sensorData;
  }
  
  return result;
}

async function loadHistoryFromServer() {
  try {
    let responseData;
    
    // Try fetch first, with fallback to XMLHttpRequest for content length mismatch
    try {
      const response = await safeFetch('/history', { cache: 'no-store' });
      if (!response.ok) {
        console.log('No historical data available from server');
        return;
      }
      responseData = await response.json();
    } catch (fetchError) {
      console.log('Fetch failed, trying XMLHttpRequest fallback:', fetchError.message);
      
      // Fallback to XMLHttpRequest for content length mismatch issues
      responseData = await new Promise((resolve, reject) => {
        const xhr = new XMLHttpRequest();
        xhr.open('GET', '/history', true);
        xhr.responseType = 'text';
        
        xhr.onload = function() {
          if (xhr.status >= 200 && xhr.status < 300) {
            try {
              const data = JSON.parse(xhr.responseText);
              resolve(data);
            } catch (parseError) {
              reject(new Error('Failed to parse JSON: ' + parseError.message));
            }
          } else {
            reject(new Error('HTTP ' + xhr.status));
          }
        };
        
        xhr.onerror = function() {
          reject(new Error('Network error'));
        };
        
        xhr.send();
      });
    }
    
    // Check if data is compressed
    if (responseData.compressed && responseData.data) {
      // Decode Base64 data using browser's built-in atob() function
      const binaryString = atob(responseData.data);
      
      // Convert binary string to Uint8Array more efficiently
      const binaryData = new Uint8Array(binaryString.length);
      for (let i = 0; i < binaryString.length; i++) {
        binaryData[i] = binaryString.charCodeAt(i) & 0xFF; // Ensure we only get the lower 8 bits
      }
      
      // Parse binary format - use the buffer directly
      const arrayBuffer = binaryData.buffer;
      responseData = parseBinaryHistory(arrayBuffer);
    }
    
    const serverTime = responseData.serverTime; // ESP32 millis() when response was sent
    const clientTime = Date.now(); // JavaScript timestamp when response was received
    
    // ESP32 timestamps are millis() values (time since boot)
    // We need to convert them to real timestamps
    const currentTime = Date.now();
    const timeOffset = currentTime - serverTime; // Calculate offset between client and server time
    
    
    // Load historical data into memory
    for (const [mac, data] of Object.entries(responseData.data)) {
      if (data && typeof data === 'object') {
        // Convert ESP32 millis() timestamps to real timestamps, then to "minutes ago" format
        const convertedData = {};
        for (const [key, points] of Object.entries(data)) {
          if (Array.isArray(points)) {
            const convertedPoints = points.map(point => {
              // Convert ESP32 millis() to real timestamp
              // serverTime is the ESP32 millis() when the response was sent
              // point.x is the ESP32 millis() when the data point was recorded
              // We need to calculate the real timestamp of the data point
              const realTimestamp = clientTime - (serverTime - point.x);
              // Convert to "minutes ago" format
              const minutesAgo = (Date.now() - realTimestamp) / (1000 * 60);
              return {
                x: realTimestamp, // Store as real timestamp for chart rendering
                y: point.y
              };
            });
            // Deduplicate and sort the converted data
            convertedData[key] = deduplicateDataPoints(convertedPoints);
          }
        }
        historyByMac.set(mac, convertedData);
        console.log(`Loaded history for ${mac}:`, convertedData);
      }
    }
    
    console.log(`Loaded ${historyByMac.size} sensor histories from server`);
    
    // Render chart with historical data
    renderChart();
  } catch (e) {
    console.log('Failed to load history from server:', e);
  }
}

// === THEME TOGGLE FUNCTIONALITY ===
function initializeThemeToggle() {
  const themeToggle = document.getElementById('themeToggle');
  const themeIcon = themeToggle?.querySelector('.theme-icon');
  
  if (!themeToggle || !themeIcon) {
    console.log('Theme toggle elements not found');
    return;
  }
  
  // Get saved theme or default to dark
  const savedTheme = localStorage.getItem('dashboard-theme') || 'dark';
  document.documentElement.setAttribute('data-theme', savedTheme);
  updateThemeIcon(themeIcon, savedTheme);
  
  themeToggle.addEventListener('click', () => {
    const currentTheme = document.documentElement.getAttribute('data-theme');
    const newTheme = currentTheme === 'dark' ? 'light' : 'dark';
    
    document.documentElement.setAttribute('data-theme', newTheme);
    localStorage.setItem('dashboard-theme', newTheme);
    updateThemeIcon(themeIcon, newTheme);
    
    console.log(`Theme switched to: ${newTheme}`);
  });
}

function updateThemeIcon(icon, theme) {
  if (theme === 'dark') {
    icon.textContent = '🌙';
  } else {
    icon.textContent = '☀️';
  }
}

// === INITIALIZE ===
async function initialize() {
  // Add iPad/Safari debugging
  console.log('Dashboard initializing...');
  console.log('User Agent:', navigator.userAgent);
  console.log('Screen size:', screen.width + 'x' + screen.height);
  console.log('Viewport size:', window.innerWidth + 'x' + window.innerHeight);
  console.log('Device pixel ratio:', window.devicePixelRatio);
  console.log('Fetch API available:', typeof fetch !== 'undefined');
  console.log('Promise available:', typeof Promise !== 'undefined');
  
  // Check for common iPad issues
  const isIPad = /iPad/.test(navigator.userAgent) || (navigator.platform === 'MacIntel' && navigator.maxTouchPoints > 1);
  if (isIPad) {
    console.log('iPad detected - applying iPad-specific optimizations');
    document.body.classList.add('ipad-device');
    
    // Test basic connectivity
    console.log('Testing basic connectivity...');
    try {
      const testResponse = await safeFetch(window.location.origin + '/ping', {
        method: 'GET',
        headers: { 'Accept': 'text/plain' }
      });
      console.log('Connectivity test result:', testResponse.status, await testResponse.text());
    } catch (connectError) {
      console.log('Connectivity test failed:', connectError);
    }
  }
  
  try {
    await loadHistoryFromServer(); // Load historical data first
  } catch (error) {
    console.error('Failed to load history from server:', error);
    // Continue initialization even if history fails
  }
  
  // Clean up any existing duplicates in all sensor data
  for (const [mac, hist] of historyByMac.entries()) {
    hist.t = deduplicateDataPoints(hist.t);
    hist.h = deduplicateDataPoints(hist.h);
    hist.p = deduplicateDataPoints(hist.p);
  }
  
  // Initialize components with defensive checks
  if (typeof initializeThemeToggle === 'function') {
    initializeThemeToggle(); // Initialize theme toggle
  }
  
  if (typeof updateChartModeOptions === 'function') {
    updateChartModeOptions();
  }
  
  if (typeof updateESP32Info === 'function') {
    updateESP32Info(); // Load ESP32 system info
  }
  
  // Initialize ESP32 background immediately
  if (typeof initializeESP32Background === 'function') {
    initializeESP32Background();
  }
  
  if (typeof tick === 'function') {
    tick(); // Start live data updates
  }
  
  if (typeof restartTimer === 'function') {
    restartTimer();
  }
  
  console.log('Dashboard initialized successfully');
}
initialize();

// Update ESP32 info every 20 seconds - with defensive check
if (typeof updateESP32Info === 'function') {
  setInterval(updateESP32Info, 20000);
} else {
  console.log('updateESP32Info function not available for interval');
}

// Reload history data every 2 minutes to keep it fresh
if (typeof loadHistoryFromServer === 'function') {
  setInterval(async () => {
    try {
      console.log('Periodic history refresh...');
      await loadHistoryFromServer();
      console.log('Periodic history refresh completed');
      // Re-render chart with fresh data
      if (typeof renderChart === 'function') {
        renderChart();
      }
    } catch (error) {
      console.log('Periodic history refresh failed:', error);
    }
  }, 120000); // 2 minutes
} else {
  console.log('loadHistoryFromServer function not available for periodic refresh');
}
