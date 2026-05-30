#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <time.h>

// ==================== CONFIGURATION ====================
const char* ssid = "🪔";
const char* password = "pollob12";

// Network test targets
const char* internetTargets[] = {"google.com", "microsoft.com", "cloudflare.com", "github.com", "amazon.com"};
const int internetTargetCount = 5;

// Telegram notifications (optional - fill to enable)
const char* telegramBotToken = "";  // Leave empty to disable
const char* telegramChatId = "";

// Timing
const unsigned long MONITOR_INTERVAL_MS = 1000;
const unsigned long SAVE_INTERVAL_MS = 60000;
const unsigned long NTP_UPDATE_INTERVAL_MS = 3600000;

const int LED_PIN = 8;

// ==================== GLOBAL STATE ====================
WebServer server(80);
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 3600, 60000);

bool gatewayOk = false;
bool dnsOk = false;
bool internetOk = false;
bool lastOverallOk = true;
unsigned long lastMonitorTime = 0;
unsigned long lastSaveTime = 0;
unsigned long lastNTPUpdate = 0;
unsigned long startTime = 0;

unsigned long totalChecks = 0;
unsigned long failedChecks = 0;

struct OutageEvent {
  unsigned long startEpoch;
  unsigned long endEpoch;
  unsigned long durationSec;
  String reason;
};
std::vector<OutageEvent> outages;
OutageEvent currentOutage = {0, 0, 0, ""};

unsigned long totalUptimeSeconds = 0;
unsigned long totalDowntimeSeconds = 0;
unsigned long longestOutageSeconds = 0;
unsigned long lastTransitionTime = 0;

// Enhanced 24-hour history (store per minute)
const int HISTORY_SIZE = 1440;
bool availabilityHistory[HISTORY_SIZE];
int historyIndex = 0;
unsigned long lastHistoryUpdate = 0;

unsigned long lastWiFiReconnectAttempt = 0;
unsigned long lastCheckDuration = 0;

// ==================== TELEGRAM NOTIFICATION ====================
void sendTelegramMessage(String message) {
  if (strlen(telegramBotToken) == 0) return;
  
  WiFiClient client;
  if (client.connect("api.telegram.org", 443)) {
    String url = "https://api.telegram.org/bot" + String(telegramBotToken) + 
                 "/sendMessage?chat_id=" + String(telegramChatId) + 
                 "&text=" + message;
    client.print(String("GET ") + url + " HTTP/1.1\r\n" +
                 "Host: api.telegram.org\r\n" +
                 "Connection: close\r\n\r\n");
    client.stop();
  }
}

// ==================== STORAGE ====================
bool saveDataToLittleFS() {
  if (millis() - lastSaveTime < SAVE_INTERVAL_MS && lastSaveTime != 0) {
    return true;
  }
  
  File file = LittleFS.open("/monitor.json", "w");
  if (!file) return false;
  
  StaticJsonDocument<24576> doc;
  doc["totalUptimeSec"] = totalUptimeSeconds;
  doc["totalDowntimeSec"] = totalDowntimeSeconds;
  doc["longestOutageSec"] = longestOutageSeconds;
  doc["startTime"] = startTime;
  doc["totalChecks"] = totalChecks;
  doc["failedChecks"] = failedChecks;
  
  JsonArray outagesArray = doc.createNestedArray("outages");
  for (auto& o : outages) {
    JsonObject obj = outagesArray.createNestedObject();
    obj["start"] = o.startEpoch;
    obj["end"] = o.endEpoch;
    obj["duration"] = o.durationSec;
    obj["reason"] = o.reason;
  }
  
  JsonArray historyArray = doc.createNestedArray("history");
  for (int i = 0; i < HISTORY_SIZE; i++) {
    historyArray.add(availabilityHistory[i]);
  }
  doc["historyIndex"] = historyIndex;
  doc["lastHistoryUpdate"] = lastHistoryUpdate;
  
  serializeJson(doc, file);
  file.close();
  lastSaveTime = millis();
  Serial.println("💾 Data saved to LittleFS");
  return true;
}

bool loadDataFromLittleFS() {
  if (!LittleFS.exists("/monitor.json")) {
    Serial.println("No saved data, starting fresh");
    return false;
  }
  
  File file = LittleFS.open("/monitor.json", "r");
  if (!file) return false;
  
  StaticJsonDocument<24576> doc;
  DeserializationError error = deserializeJson(doc, file);
  file.close();
  
  if (error) return false;
  
  totalUptimeSeconds = doc["totalUptimeSec"] | 0;
  totalDowntimeSeconds = doc["totalDowntimeSec"] | 0;
  longestOutageSeconds = doc["longestOutageSec"] | 0;
  startTime = doc["startTime"] | 0;
  totalChecks = doc["totalChecks"] | 0;
  failedChecks = doc["failedChecks"] | 0;
  
  JsonArray outagesArray = doc["outages"];
  outages.clear();
  for (JsonObject obj : outagesArray) {
    OutageEvent ev;
    ev.startEpoch = obj["start"];
    ev.endEpoch = obj["end"];
    ev.durationSec = obj["duration"];
    ev.reason = obj["reason"] | "Unknown";
    outages.push_back(ev);
  }
  
  JsonArray historyArray = doc["history"];
  if (historyArray.size() == HISTORY_SIZE) {
    for (int i = 0; i < HISTORY_SIZE; i++) {
      availabilityHistory[i] = historyArray[i] | true;
    }
    historyIndex = doc["historyIndex"] | 0;
    lastHistoryUpdate = doc["lastHistoryUpdate"] | 0;
  } else {
    for (int i = 0; i < HISTORY_SIZE; i++) availabilityHistory[i] = true;
    historyIndex = 0;
    lastHistoryUpdate = 0;
  }
  
  Serial.printf("📂 Loaded: %d outages, uptime=%lus\n", outages.size(), totalUptimeSeconds);
  return true;
}

void updateAvailabilityHistory(bool isAvailable) {
  unsigned long nowSec = timeClient.getEpochTime();
  if (lastHistoryUpdate == 0 || nowSec < 1000000000) {
    lastHistoryUpdate = nowSec;
    return;
  }
  
  unsigned long minutesPassed = (nowSec - lastHistoryUpdate) / 60;
  if (minutesPassed >= 1) {
    for (unsigned long i = 0; i < minutesPassed && i < 60; i++) {
      availabilityHistory[historyIndex] = isAvailable;
      historyIndex = (historyIndex + 1) % HISTORY_SIZE;
    }
    lastHistoryUpdate = nowSec;
  }
}

// ==================== NETWORK TESTS ====================
bool pingTarget(const char* target) {
  WiFiClient client;
  client.setTimeout(500);
  if (client.connect(target, 80)) {
    client.stop();
    return true;
  }
  return false;
}

bool checkGateway() {
  IPAddress gatewayIP = WiFi.gatewayIP();
  if (gatewayIP != INADDR_NONE && gatewayIP != IPAddress(0,0,0,0)) {
    WiFiClient client;
    client.setTimeout(500);
    if (client.connect(gatewayIP, 80)) {
      client.stop();
      return true;
    }
  }
  return false;
}

bool checkDNS() {
  IPAddress result;
  return (WiFi.hostByName("8.8.8.8", result) == 1);
}

bool checkInternet() {
  for (int i = 0; i < internetTargetCount; i++) {
    if (pingTarget(internetTargets[i])) return true;
  }
  return false;
}

void performNetworkTests() {
  unsigned long startCheck = micros();
  
  gatewayOk = checkGateway();
  if (gatewayOk) {
    dnsOk = checkDNS();
    if (dnsOk) internetOk = checkInternet();
    else internetOk = false;
  } else {
    dnsOk = false;
    internetOk = false;
  }
  
  lastCheckDuration = micros() - startCheck;
  totalChecks++;
}

void updateStatistics(bool isOverallOk) {
  unsigned long now = timeClient.getEpochTime();
  if (now < 1000000000) return;
  
  if (isOverallOk) {
    if (!lastOverallOk) {
      if (currentOutage.startEpoch != 0) {
        currentOutage.endEpoch = now;
        currentOutage.durationSec = now - currentOutage.startEpoch;
        
        // Determine failure reason
        if (!gatewayOk) currentOutage.reason = "Gateway Failed";
        else if (!dnsOk) currentOutage.reason = "DNS Failed";
        else if (!internetOk) currentOutage.reason = "Internet Failed";
        
        outages.push_back(currentOutage);
        
        if (currentOutage.durationSec > longestOutageSeconds) {
          longestOutageSeconds = currentOutage.durationSec;
        }
        
        totalDowntimeSeconds += currentOutage.durationSec;
        failedChecks += currentOutage.durationSec;
        
        // Send notification
        char msg[200];
        snprintf(msg, sizeof(msg), "🔴 NETWORK OUTAGE ENDED\nDuration: %lu minutes %lu seconds\nReason: %s",
                 currentOutage.durationSec / 60, currentOutage.durationSec % 60, currentOutage.reason.c_str());
        sendTelegramMessage(msg);
        
        currentOutage = {0, 0, 0, ""};
        saveDataToLittleFS();
      }
      lastTransitionTime = now;
    }
    if (lastTransitionTime > 0) totalUptimeSeconds += (now - lastTransitionTime);
    else totalUptimeSeconds++;
    lastTransitionTime = now;
  } else {
    if (lastOverallOk) {
      currentOutage.startEpoch = now;
      
      // Send outage start notification
      char msg[100];
      char timeStr[30];
      time_t now_t = now;
      strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", localtime(&now_t));
      snprintf(msg, sizeof(msg), "⚠️ NETWORK OUTAGE STARTED\nTime: %s", timeStr);
      sendTelegramMessage(msg);
      
      lastTransitionTime = now;
    }
    if (lastTransitionTime > 0) totalDowntimeSeconds += (now - lastTransitionTime);
    else totalDowntimeSeconds++;
    failedChecks++;
    lastTransitionTime = now;
  }
  
  updateAvailabilityHistory(isOverallOk);
  lastOverallOk = isOverallOk;
}

void handleWiFiReconnect() {
  if (WiFi.status() != WL_CONNECTED) {
    unsigned long now = millis();
    if (now - lastWiFiReconnectAttempt > 30000) {
      lastWiFiReconnectAttempt = now;
      Serial.println("🔄 WiFi reconnecting...");
      WiFi.disconnect();
      WiFi.begin(ssid, password);
    }
  }
}

// ==================== WEB HANDLERS ====================
void handleRoot() {
  String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset='UTF-8'>
<meta name='viewport' content='width=device-width,initial-scale=1'>
<title>ESP32-C3 Network Monitor</title>
<script src='https://cdn.jsdelivr.net/npm/chart.js@4.4.0/dist/chart.umd.min.js'></script>
<style>
  * { margin: 0; padding: 0; box-sizing: border-box; }
  body {
    background: linear-gradient(135deg, #0a0e1a 0%, #0f1422 100%);
    color: #e5e7eb;
    font-family: 'Segoe UI', system-ui, -apple-system, sans-serif;
    padding: 20px;
    min-height: 100vh;
  }
  .dashboard { max-width: 1400px; margin: 0 auto; }
  
  .header {
    background: linear-gradient(135deg, #1a1f2e 0%, #111827 100%);
    border-radius: 16px;
    padding: 20px 30px;
    margin-bottom: 25px;
    border: 1px solid #2d3748;
    box-shadow: 0 4px 20px rgba(0,0,0,0.3);
  }
  h1 {
    font-size: 1.8rem;
    display: flex;
    align-items: center;
    gap: 15px;
    flex-wrap: wrap;
  }
  .badge {
    background: #1f2937;
    padding: 5px 15px;
    border-radius: 20px;
    font-size: 0.8rem;
    font-weight: normal;
  }
  .status-led {
    width: 16px;
    height: 16px;
    border-radius: 50%;
    display: inline-block;
    animation: pulse 2s infinite;
  }
  @keyframes pulse {
    0%, 100% { opacity: 1; }
    50% { opacity: 0.5; }
  }
  .online { background: #10b981; box-shadow: 0 0 10px #10b981; }
  .offline { background: #ef4444; box-shadow: 0 0 10px #ef4444; }
  
  .stats-grid {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(200px, 1fr));
    gap: 20px;
    margin-bottom: 25px;
  }
  .stat-card {
    background: #111827;
    border: 1px solid #2d3748;
    border-radius: 16px;
    padding: 20px;
    transition: transform 0.2s, box-shadow 0.2s;
  }
  .stat-card:hover {
    transform: translateY(-2px);
    box-shadow: 0 8px 25px rgba(0,0,0,0.3);
  }
  .stat-label {
    font-size: 0.75rem;
    text-transform: uppercase;
    letter-spacing: 1px;
    color: #9ca3af;
    margin-bottom: 10px;
  }
  .stat-value {
    font-size: 1.8rem;
    font-weight: bold;
    font-family: monospace;
  }
  .stat-value.small { font-size: 1.2rem; }
  .status-ok { color: #10b981; }
  .status-fail { color: #ef4444; }
  
  .status-grid {
    display: grid;
    grid-template-columns: repeat(4, 1fr);
    gap: 20px;
    margin-bottom: 25px;
  }
  .status-card {
    background: #111827;
    border: 2px solid #2d3748;
    border-radius: 16px;
    padding: 20px;
    text-align: center;
    transition: all 0.3s;
  }
  .status-card.online { border-color: #10b981; background: rgba(16,185,129,0.05); }
  .status-card.offline { border-color: #ef4444; background: rgba(239,68,68,0.05); }
  .status-icon { font-size: 3rem; margin-bottom: 10px; }
  .status-title { font-size: 0.9rem; color: #9ca3af; margin-bottom: 8px; }
  .status-text { font-size: 1.2rem; font-weight: bold; }
  
  .chart-container {
    background: #111827;
    border: 1px solid #2d3748;
    border-radius: 16px;
    padding: 20px;
    margin-bottom: 25px;
  }
  .chart-container h3 {
    margin-bottom: 15px;
    color: #9ca3af;
    font-size: 0.9rem;
  }
  canvas { max-height: 300px; }
  
  .table-container {
    background: #111827;
    border: 1px solid #2d3748;
    border-radius: 16px;
    padding: 20px;
    margin-bottom: 25px;
  }
  .table-header {
    display: flex;
    justify-content: space-between;
    align-items: center;
    margin-bottom: 15px;
    flex-wrap: wrap;
    gap: 10px;
  }
  table {
    width: 100%;
    border-collapse: collapse;
  }
  th, td {
    padding: 12px;
    text-align: left;
    border-bottom: 1px solid #2d3748;
  }
  th {
    color: #9ca3af;
    font-weight: 600;
    font-size: 0.8rem;
  }
  .btn {
    background: #3b82f6;
    border: none;
    color: white;
    padding: 8px 16px;
    border-radius: 8px;
    cursor: pointer;
    font-size: 0.85rem;
    transition: all 0.2s;
  }
  .btn:hover { background: #2563eb; transform: scale(1.02); }
  .btn-danger { background: #ef4444; }
  .btn-danger:hover { background: #dc2626; }
  .footer {
    text-align: center;
    color: #6b7280;
    font-size: 0.75rem;
    margin-top: 20px;
  }
  @media (max-width: 768px) {
    .status-grid { grid-template-columns: repeat(2, 1fr); }
    .stats-grid { grid-template-columns: repeat(2, 1fr); }
    .stat-value { font-size: 1.2rem; }
  }
</style>
</head>
<body>
<div class='dashboard'>
  <div class='header'>
    <h1>
      <span class='status-led' id='led-indicator'></span>
      ESP32-C3 Network Monitor
      <span class='badge'>⚡ 1-SECOND CHECKS</span>
      <span class='badge' id='check-counter'>-- checks</span>
    </h1>
  </div>
  
  <div class='status-grid'>
    <div class='status-card' id='overall-card'>
      <div class='status-icon'>🌐</div>
      <div class='status-title'>OVERALL STATUS</div>
      <div class='status-text' id='overall-status'>--</div>
    </div>
    <div class='status-card' id='gateway-card'>
      <div class='status-icon'>🚪</div>
      <div class='status-title'>GATEWAY</div>
      <div class='status-text' id='gateway-status'>--</div>
    </div>
    <div class='status-card' id='dns-card'>
      <div class='status-icon'>🔍</div>
      <div class='status-title'>DNS</div>
      <div class='status-text' id='dns-status'>--</div>
    </div>
    <div class='status-card' id='internet-card'>
      <div class='status-icon'>🌍</div>
      <div class='status-title'>INTERNET</div>
      <div class='status-text' id='internet-status'>--</div>
    </div>
  </div>
  
  <div class='stats-grid'>
    <div class='stat-card'><div class='stat-label'>📊 UPTIME %</div><div class='stat-value' id='uptime-percent'>--</div></div>
    <div class='stat-card'><div class='stat-label'>⏱️ TOTAL UPTIME</div><div class='stat-value small' id='total-uptime'>--</div></div>
    <div class='stat-card'><div class='stat-label'>⚠️ LONGEST OUTAGE</div><div class='stat-value small' id='longest-outage'>--</div></div>
    <div class='stat-card'><div class='stat-label'>📈 AVG OUTAGE</div><div class='stat-value small' id='avg-outage'>--</div></div>
    <div class='stat-card'><div class='stat-label'>🔢 TOTAL CHECKS</div><div class='stat-value small' id='total-checks'>--</div></div>
    <div class='stat-card'><div class='stat-label'>❌ FAILED CHECKS</div><div class='stat-value small' id='failed-checks'>--</div></div>
    <div class='stat-card'><div class='stat-label'>✅ AVAILABILITY</div><div class='stat-value small' id='availability'>--</div></div>
    <div class='stat-card'><div class='stat-label'>⚡ CHECK TIME</div><div class='stat-value small' id='check-time'>--</div></div>
  </div>
  
  <div class='chart-container'>
    <h3>📈 24-HOUR AVAILABILITY HISTORY</h3>
    <canvas id='availabilityChart'></canvas>
  </div>
  
  <div class='table-container'>
    <div class='table-header'>
      <h3>📋 OUTAGE HISTORY</h3>
      <div>
        <button class='btn' onclick='refreshData()'>⟳ Refresh</button>
        <button class='btn' onclick='exportData()'>📥 Export CSV</button>
        <button class='btn btn-danger' onclick='clearHistory()'>🗑️ Clear History</button>
      </div>
    </div>
    <div style='overflow-x:auto; max-height: 400px; overflow-y: auto;'>
      <table id='outage-table'>
        <thead><tr><th>#</th><th>Start Time</th><th>End Time</th><th>Duration</th><th>Reason</th></tr></thead>
        <tbody><tr><td colspan='5' style='text-align:center;'>Loading...</td></tr></tbody>
      </table>
    </div>
  </div>
  
  <div class='footer' id='last-update'></div>
</div>

<script>
let chart;
let currentData = null;

function formatDuration(sec) {
  if (!sec || sec === 0) return 'None';
  let days = Math.floor(sec / 86400);
  let hours = Math.floor((sec % 86400) / 3600);
  let minutes = Math.floor((sec % 3600) / 60);
  let seconds = sec % 60;
  if (days > 0) return days + 'd ' + hours + 'h';
  if (hours > 0) return hours + 'h ' + minutes + 'm';
  if (minutes > 0) return minutes + 'm ' + seconds + 's';
  return seconds + 's';
}

function formatUptime(sec) {
  let days = Math.floor(sec / 86400);
  let hours = Math.floor((sec % 86400) / 3600);
  let minutes = Math.floor((sec % 3600) / 60);
  return days + 'd ' + hours + 'h ' + minutes + 'm';
}

function refreshData() {
  fetch('/api/status')
    .then(r => r.json())
    .then(data => {
      currentData = data;
      
      const isOnline = data.overall;
      document.getElementById('overall-status').innerHTML = isOnline ? '<span class="status-ok">✓ ONLINE</span>' : '<span class="status-fail">✗ OFFLINE</span>';
      document.getElementById('gateway-status').innerHTML = data.gateway ? '<span class="status-ok">✓ OK</span>' : '<span class="status-fail">✗ FAIL</span>';
      document.getElementById('dns-status').innerHTML = data.dns ? '<span class="status-ok">✓ OK</span>' : '<span class="status-fail">✗ FAIL</span>';
      document.getElementById('internet-status').innerHTML = data.internet ? '<span class="status-ok">✓ OK</span>' : '<span class="status-fail">✗ FAIL</span>';
      
      document.getElementById('overall-card').className = 'status-card ' + (isOnline ? 'online' : 'offline');
      document.getElementById('gateway-card').className = 'status-card ' + (data.gateway ? 'online' : 'offline');
      document.getElementById('dns-card').className = 'status-card ' + (data.dns ? 'online' : 'offline');
      document.getElementById('internet-card').className = 'status-card ' + (data.internet ? 'online' : 'offline');
      
      document.getElementById('led-indicator').className = 'status-led ' + (isOnline ? 'online' : 'offline');
      
      document.getElementById('uptime-percent').innerHTML = data.uptimePercent.toFixed(2) + '%';
      document.getElementById('total-uptime').innerHTML = formatUptime(data.totalUptime);
      document.getElementById('longest-outage').innerHTML = formatDuration(data.longestOutage);
      document.getElementById('avg-outage').innerHTML = formatDuration(data.avgOutage);
      document.getElementById('total-checks').innerHTML = data.totalChecks.toLocaleString();
      document.getElementById('failed-checks').innerHTML = data.failedChecks.toLocaleString();
      let availability = ((data.totalChecks - data.failedChecks) / data.totalChecks * 100).toFixed(2);
      document.getElementById('availability').innerHTML = availability + '%';
      document.getElementById('check-time').innerHTML = (data.checkDuration / 1000).toFixed(1) + ' ms';
      document.getElementById('check-counter').innerHTML = data.totalChecks.toLocaleString() + ' checks';
      
      const tbody = document.querySelector('#outage-table tbody');
      if (data.outages && data.outages.length > 0) {
        let html = '';
        const reversed = [...data.outages].reverse();
        for (let i = 0; i < reversed.length; i++) {
          const o = reversed[i];
          const startDate = new Date(o.start * 1000).toLocaleString();
          const endDate = new Date(o.end * 1000).toLocaleString();
          html += `<tr><td>${reversed.length - i}</td><td>${startDate}</td><td>${endDate}</td><td>${formatDuration(o.duration)}</td><td><span style="color:#f59e0b;">⚠️ ${o.reason || 'Unknown'}</span></td></tr>`;
        }
        tbody.innerHTML = html;
      } else {
        tbody.innerHTML = '<tr><td colspan="5" style="text-align:center;">✅ No outages recorded</td></tr>';
      }
      
      if (chart) chart.destroy();
      const ctx = document.getElementById('availabilityChart').getContext('2d');
      const gradient = ctx.createLinearGradient(0, 0, 0, 300);
      gradient.addColorStop(0, 'rgba(16, 185, 129, 0.4)');
      gradient.addColorStop(1, 'rgba(16, 185, 129, 0.05)');
      
      chart = new Chart(ctx, {
        type: 'line',
        data: {
          labels: data.historyLabels,
          datasets: [{
            label: 'Network Status',
            data: data.historyData,
            borderColor: '#10b981',
            backgroundColor: gradient,
            borderWidth: 3,
            fill: true,
            tension: 0.2,
            pointRadius: 0,
            pointHoverRadius: 5,
            pointBackgroundColor: '#10b981'
          }]
        },
        options: {
          responsive: true,
          maintainAspectRatio: true,
          plugins: {
            legend: { labels: { color: '#9ca3af', font: { size: 11 } } },
            tooltip: { callbacks: { label: function(context) { return context.raw === 1 ? '✅ Network Available' : '❌ Network Outage'; } } }
          },
          scales: {
            y: { min: -0.1, max: 1.1, ticks: { stepSize: 1, callback: function(val) { return val === 1 ? '● Up' : '○ Down'; }, color: '#9ca3af' }, grid: { color: '#2d3748' } },
            x: { ticks: { color: '#9ca3af', maxRotation: 45 }, grid: { color: '#2d3748' } }
          }
        }
      });
      
      document.getElementById('last-update').innerHTML = 'Last updated: ' + new Date().toLocaleString() + ' | Monitoring every 1 second';
    })
    .catch(e => console.error('Error:', e));
}

function exportData() {
  window.location.href = '/api/export';
}

function clearHistory() {
  if (confirm('⚠️ WARNING: This will delete ALL outage history. This cannot be undone. Continue?')) {
    fetch('/api/clear', { method: 'POST' })
      .then(() => refreshData());
  }
}

refreshData();
setInterval(refreshData, 2000);
</script>
</body>
</html>
)rawliteral";
  server.send(200, "text/html", html);
}

void handleAPIStatus() {
  unsigned long totalTime = totalUptimeSeconds + totalDowntimeSeconds;
  float uptimePercent = (totalTime > 0) ? (totalUptimeSeconds * 100.0 / totalTime) : 100.0;
  
  unsigned long avgOutage = 0;
  if (outages.size() > 0) {
    unsigned long totalDur = 0;
    for (auto& o : outages) totalDur += o.durationSec;
    avgOutage = totalDur / outages.size();
  }
  
  StaticJsonDocument<32768> doc;
  doc["overall"] = lastOverallOk && gatewayOk && dnsOk && internetOk;
  doc["gateway"] = gatewayOk;
  doc["dns"] = dnsOk;
  doc["internet"] = internetOk;
  doc["uptimePercent"] = uptimePercent;
  doc["totalUptime"] = totalUptimeSeconds;
  doc["longestOutage"] = longestOutageSeconds;
  doc["avgOutage"] = avgOutage;
  doc["totalChecks"] = totalChecks;
  doc["failedChecks"] = failedChecks;
  doc["checkDuration"] = lastCheckDuration;
  
  JsonArray outagesArray = doc.createNestedArray("outages");
  int startIdx = outages.size() > 100 ? outages.size() - 100 : 0;
  for (int i = startIdx; i < outages.size(); i++) {
    JsonObject obj = outagesArray.createNestedObject();
    obj["start"] = outages[i].startEpoch;
    obj["end"] = outages[i].endEpoch;
    obj["duration"] = outages[i].durationSec;
    obj["reason"] = outages[i].reason;
  }
  
  JsonArray historyData = doc.createNestedArray("historyData");
  JsonArray historyLabels = doc.createNestedArray("historyLabels");
  int step = HISTORY_SIZE / 48;
  for (int i = 0; i < 48; i++) {
    int idx = (historyIndex + i * step) % HISTORY_SIZE;
    historyData.add(availabilityHistory[idx] ? 1 : 0);
    historyLabels.add(String(i * 30) + ":00");
  }
  
  String response;
  serializeJson(doc, response);
  server.send(200, "application/json", response);
}

void handleExportCSV() {
  String csv = "Start Time,End Time,Duration (seconds),Duration (min:sec),Reason\n";
  for (auto& o : outages) {
    char startStr[30], endStr[30];
    time_t startT = o.startEpoch;
    time_t endT = o.endEpoch;
    struct tm* tmInfo = localtime(&startT);
    strftime(startStr, sizeof(startStr), "%Y-%m-%d %H:%M:%S", tmInfo);
    tmInfo = localtime(&endT);
    strftime(endStr, sizeof(endStr), "%Y-%m-%d %H:%M:%S", tmInfo);
    csv += String(startStr) + "," + String(endStr) + "," + 
           String(o.durationSec) + "," + String(o.durationSec/60) + ":" + String(o.durationSec%60) + "," +
           o.reason + "\n";
  }
  server.send(200, "text/csv", csv);
}

void handleClearHistory() {
  outages.clear();
  totalDowntimeSeconds = 0;
  longestOutageSeconds = 0;
  failedChecks = 0;
  saveDataToLittleFS();
  server.send(200, "application/json", "{\"status\":\"cleared\"}");
}

// ==================== SETUP ====================
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n╔══════════════════════════════════════════════╗");
  Serial.println("║     ESP32-C3 Network Monitor v3.0          ║");
  Serial.println("║       Enhanced with Persistent Storage      ║");
  Serial.println("╚══════════════════════════════════════════════╝\n");
  
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
  
  if (!LittleFS.begin(false)) {
    Serial.println("Mounting LittleFS...");
    if (!LittleFS.begin(true)) Serial.println("Failed!");
  }
  
  loadDataFromLittleFS();
  
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 40) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n✓ WiFi connected!");
    Serial.print("📡 IP: http://");
    Serial.println(WiFi.localIP());
    digitalWrite(LED_PIN, HIGH);
  }
  
  timeClient.begin();
  timeClient.setTimeOffset(3600);
  timeClient.update();
  
  startTime = timeClient.getEpochTime();
  if (startTime < 1000000000) startTime = 1700000000;
  lastTransitionTime = startTime;
  
  if (lastHistoryUpdate == 0) {
    for (int i = 0; i < HISTORY_SIZE; i++) availabilityHistory[i] = true;
    lastHistoryUpdate = startTime;
  }
  
  server.on("/", handleRoot);
  server.on("/api/status", handleAPIStatus);
  server.on("/api/export", handleExportCSV);
  server.on("/api/clear", HTTP_POST, handleClearHistory);
  
  server.begin();
  Serial.println("🌐 HTTP server started on port 80");
  Serial.println("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
  Serial.println("⚡ MONITORING ACTIVE - CHECKING EVERY 1 SECOND");
  Serial.println("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");
  
  lastMonitorTime = millis();
  lastSaveTime = millis();
}

// ==================== MAIN LOOP ====================
void loop() {
  server.handleClient();
  handleWiFiReconnect();

  // Perform network monitoring at regular intervals
  unsigned long now = millis();
  if (now - lastMonitorTime >= MONITOR_INTERVAL_MS) {
    performNetworkTests();
    bool isOverallOk = gatewayOk && dnsOk && internetOk;
    updateStatistics(isOverallOk);
    lastMonitorTime = now;
  }

  // Update NTP time periodically
  if (now - lastNTPUpdate >= NTP_UPDATE_INTERVAL_MS) {
    timeClient.update();
    lastNTPUpdate = now;
  }
}
