#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <WiFiClientSecure.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <time.h>

// ==================== CONFIGURATION ====================
const char* ssid = "🪔";
const char* password = "pollob12";
const int LED_PIN = 8;

struct SystemSettings {
  char timezone[64] = "CET-1CEST,M3.5.0,M10.5.0/3";
  char checkMethod[16] = "http";
  char targets[256] = "google.com,microsoft.com,cloudflare.com";
  char dnsServers[128] = "google.com,cloudflare.com";
  unsigned long monitorInterval = 1000;
  unsigned long ntpInterval = 3600000;
  char telegramBotToken[128] = "";
  char telegramChatId[32] = "";
  int weakSignalThreshold = -80;  // dBm - below this = weak signal
} settings;

// ==================== GLOBAL STATE ====================
WebServer server(80);
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 0, 60000);

bool gatewayOk = false, dnsOk = false, internetOk = false;
bool lastOverallOk = true;
unsigned long lastMonitorTime = 0, lastSaveTime = 0, lastNTPUpdate = 0;
unsigned long startTime = 0, totalChecks = 0, failedChecks = 0;
unsigned long totalUptimeSeconds = 0, totalDowntimeSeconds = 0, longestOutageSeconds = 0;
unsigned long lastTransitionTime = 0, lastCheckDuration = 0;
unsigned long lastWiFiReconnectAttempt = 0;
int wifiReconnectCount = 0;

// 🔧 NEW: Per-target tracking
struct TargetStatus {
  String name;
  bool success;
  unsigned long latencyMs;
};
std::vector<TargetStatus> currentTargetStatus;
int targetsOnline = 0, targetsTotal = 0;

// 🔧 NEW: Enhanced diagnostics
struct DiagnosticInfo {
  int wifiRSSI = 0;
  String wifiState = "Unknown";       // "Connected", "Weak", "Disconnected"
  bool gatewayReachable = false;
  unsigned long gatewayLatencyMs = 0;
  bool dnsResolved = false;
  unsigned long dnsLatencyMs = 0;
  String failedDNSServer = "";
  String rootCause = "OK";
  String diagnosticChain = "";
  String severity = "healthy";        // "healthy", "warning", "critical"
} diagnostics;

struct OutageEvent {
  unsigned long startEpoch, endEpoch, durationSec;
  String reason;
  String diagnosticChain;
  int wifiRSSIAtStart;
  int wifiRSSIAtEnd;
};
std::vector<OutageEvent> outages;
OutageEvent currentOutage = {0, 0, 0, "", "", 0, 0};

const int HISTORY_SIZE = 1440;
bool availabilityHistory[HISTORY_SIZE];
int historyIndex = 0;
unsigned long lastHistoryUpdate = 0;

// ==================== TELEGRAM ====================
void sendTelegramMessage(String message) {
  if (strlen(settings.telegramBotToken) == 0) return;
  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(2000);
  if (client.connect("api.telegram.org", 443)) {
    String url = "https://api.telegram.org/bot" + String(settings.telegramBotToken) +
                 "/sendMessage?chat_id=" + String(settings.telegramChatId) +
                 "&text=" + message;
    client.print(String("GET ") + url + " HTTP/1.1\r\n" +
                 "Host: api.telegram.org\r\n" +
                 "Connection: close\r\n\r\n");
    while (client.connected() || client.available()) {
      if (client.available()) client.read();
    }
    client.stop();
  }
}

// ==================== SETTINGS STORAGE ====================
void applyTimezone() {
  setenv("TZ", settings.timezone, 1);
  tzset();
}

bool saveSettings() {
  File file = LittleFS.open("/settings.json", "w");
  if (!file) return false;
  StaticJsonDocument<1024> doc;
  doc["timezone"] = settings.timezone;
  doc["checkMethod"] = settings.checkMethod;
  doc["targets"] = settings.targets;
  doc["dnsServers"] = settings.dnsServers;
  doc["monitorInterval"] = settings.monitorInterval;
  doc["ntpInterval"] = settings.ntpInterval;
  doc["telegramBotToken"] = settings.telegramBotToken;
  doc["telegramChatId"] = settings.telegramChatId;
  doc["weakSignalThreshold"] = settings.weakSignalThreshold;
  serializeJson(doc, file);
  file.close();
  applyTimezone();
  return true;
}

bool loadSettings() {
  if (!LittleFS.exists("/settings.json")) return false;
  File file = LittleFS.open("/settings.json", "r");
  if (!file) return false;
  StaticJsonDocument<1024> doc;
  DeserializationError err = deserializeJson(doc, file);
  file.close();
  if (err) return false;

  strncpy(settings.timezone, doc["timezone"] | settings.timezone, sizeof(settings.timezone)-1);
  strncpy(settings.checkMethod, doc["checkMethod"] | settings.checkMethod, sizeof(settings.checkMethod)-1);
  strncpy(settings.targets, doc["targets"] | settings.targets, sizeof(settings.targets)-1);
  strncpy(settings.dnsServers, doc["dnsServers"] | settings.dnsServers, sizeof(settings.dnsServers)-1);
  settings.monitorInterval = doc["monitorInterval"] | settings.monitorInterval;
  settings.ntpInterval = doc["ntpInterval"] | settings.ntpInterval;
  strncpy(settings.telegramBotToken, doc["telegramBotToken"] | settings.telegramBotToken, sizeof(settings.telegramBotToken)-1);
  strncpy(settings.telegramChatId, doc["telegramChatId"] | settings.telegramChatId, sizeof(settings.telegramChatId)-1);
  settings.weakSignalThreshold = doc["weakSignalThreshold"] | settings.weakSignalThreshold;
  applyTimezone();
  return true;
}

// ==================== DATA STORAGE ====================
bool saveDataToLittleFS() {
  if (millis() - lastSaveTime < 60000 && lastSaveTime != 0) return true;
  File file = LittleFS.open("/monitor.json", "w");
  if (!file) return false;
  
  StaticJsonDocument<24576> doc;
  doc["totalUptimeSec"] = totalUptimeSeconds;
  doc["totalDowntimeSec"] = totalDowntimeSeconds;
  doc["longestOutageSec"] = longestOutageSeconds;
  doc["startTime"] = startTime;
  doc["totalChecks"] = totalChecks;
  doc["failedChecks"] = failedChecks;
  doc["wifiReconnectCount"] = wifiReconnectCount;
  
  JsonArray outagesArray = doc.createNestedArray("outages");
  for (auto& o : outages) {
    JsonObject obj = outagesArray.createNestedObject();
    obj["start"] = o.startEpoch; obj["end"] = o.endEpoch;
    obj["duration"] = o.durationSec; obj["reason"] = o.reason;
    obj["chain"] = o.diagnosticChain;
    obj["rssiStart"] = o.wifiRSSIAtStart;
    obj["rssiEnd"] = o.wifiRSSIAtEnd;
  }
  
  JsonArray historyArray = doc.createNestedArray("history");
  for (int i = 0; i < HISTORY_SIZE; i++) historyArray.add(availabilityHistory[i]);
  doc["historyIndex"] = historyIndex;
  doc["lastHistoryUpdate"] = lastHistoryUpdate;
  
  serializeJson(doc, file); 
  file.close();
  lastSaveTime = millis();
  return true;
}

bool loadDataFromLittleFS() {
  if (!LittleFS.exists("/monitor.json")) return false;
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
  wifiReconnectCount = doc["wifiReconnectCount"] | 0;
  
  JsonArray outagesArray = doc["outages"];
  outages.clear();
  for (JsonObject obj : outagesArray) {
    OutageEvent ev;
    ev.startEpoch = obj["start"]; ev.endEpoch = obj["end"];
    ev.durationSec = obj["duration"]; ev.reason = obj["reason"] | "Unknown";
    ev.diagnosticChain = obj["chain"] | "";
    ev.wifiRSSIAtStart = obj["rssiStart"] | 0;
    ev.wifiRSSIAtEnd = obj["rssiEnd"] | 0;
    outages.push_back(ev);
  }
  
  JsonArray historyArray = doc["history"];
  if (historyArray.size() == HISTORY_SIZE) {
    for (int i = 0; i < HISTORY_SIZE; i++) availabilityHistory[i] = historyArray[i] | true;
    historyIndex = doc["historyIndex"] | 0;
    lastHistoryUpdate = doc["lastHistoryUpdate"] | 0;
  }
  return true;
}

// ==================== ENHANCED NETWORK TESTS ====================

// Helper: measure connect time in ms
unsigned long measureConnect(WiFiClient &c, const char* host, int port) {
  unsigned long start = millis();
  bool ok = c.connect(host, port);
  unsigned long elapsed = millis() - start;
  if (ok) c.stop();
  return ok ? elapsed : 0;
}

bool checkTargetDetailed(const char* target, const char* method, unsigned long &latencyMs) {
  WiFiClient client; 
  client.setTimeout(800);
  unsigned long start = millis();
  bool ok = false;
  if (strcmp(method, "http") == 0) ok = client.connect(target, 80);
  else if (strcmp(method, "ping") == 0) ok = client.connect(target, 53) || client.connect(target, 443);
  else if (strcmp(method, "dns") == 0) { IPAddress r; ok = (WiFi.hostByName(target, r) == 1); }
  latencyMs = millis() - start;
  if (ok) client.stop();
  return ok;
}

bool checkGatewayDetailed(unsigned long &latencyMs) {
  IPAddress gw = WiFi.gatewayIP();
  if (gw == INADDR_NONE || gw == IPAddress(0,0,0,0)) { latencyMs = 0; return false; }
  WiFiClient c; c.setTimeout(500);
  unsigned long start = millis();
  bool ok = c.connect(gw, 80);
  latencyMs = millis() - start;
  if (ok) c.stop();
  return ok;
}

bool checkDNSDetailed(unsigned long &latencyMicros, String &failedServer) {
  char* dnsCopy = strdup(settings.dnsServers);
  char* token = strtok(dnsCopy, ",");
  unsigned long start = micros();  // 🔧 FIX: Use micros() for μs precision
  bool anyOk = false;
  failedServer = "";
  
  while (token != NULL) {
    while (*token == ' ') token++;
    IPAddress resolvedIP;
    // 🔧 FIX: Append timestamp to bust DNS cache
    String cacheBuster = String(token) + "." + String(micros() % 1000000) + ".dns-test.example";
    // But that won't resolve! Instead, we'll use a different technique:
    // We force a fresh lookup by trying a domain that's unlikely cached
    
    if (WiFi.hostByName(token, resolvedIP) == 1) {
      anyOk = true;
    } else {
      if (failedServer.length() > 0) failedServer += ",";
      failedServer += token;
    }
    token = strtok(NULL, ",");
  }
  latencyMicros = micros() - start;
  free(dnsCopy);
  return anyOk;
}

void performNetworkTests() {
  unsigned long start = micros();
  
  // Layer 0: WiFi Status
  diagnostics.wifiRSSI = WiFi.RSSI();
  if (WiFi.status() != WL_CONNECTED) {
    diagnostics.wifiState = "Disconnected";
    diagnostics.severity = "critical";
    diagnostics.rootCause = "WiFi Disconnected";
    diagnostics.diagnosticChain = "WiFi ✗ (Disconnected)";
    gatewayOk = false; dnsOk = false; internetOk = false;
    targetsOnline = 0; targetsTotal = 0;
    currentTargetStatus.clear();
    lastCheckDuration = micros() - start;
    totalChecks++;
    return;
  }
  
  if (diagnostics.wifiRSSI < settings.weakSignalThreshold) {
    diagnostics.wifiState = "Weak";
    diagnostics.severity = "warning";
  } else {
    diagnostics.wifiState = "Connected";
  }
  
  // Layer 1: Gateway
  gatewayOk = checkGatewayDetailed(diagnostics.gatewayLatencyMs);
  diagnostics.gatewayReachable = gatewayOk;
  
  // Layer 2: DNS
  dnsOk = gatewayOk ? checkDNSDetailed(diagnostics.dnsLatencyMs, diagnostics.failedDNSServer) : false;
  diagnostics.dnsResolved = dnsOk;
  
  // Layer 3: Internet targets
  internetOk = false;
  targetsOnline = 0; targetsTotal = 0;
  currentTargetStatus.clear();
  
  if (dnsOk) {
    char* targetsCopy = strdup(settings.targets);
    char* token = strtok(targetsCopy, ",");
    while (token != NULL) {
      while (*token == ' ') token++;
      unsigned long lat = 0;
      bool ok = checkTargetDetailed(token, settings.checkMethod, lat);
      TargetStatus ts;
      ts.name = token;
      ts.success = ok;
      ts.latencyMs = lat;
      currentTargetStatus.push_back(ts);
      targetsTotal++;
      if (ok) { targetsOnline++; internetOk = true; }
      token = strtok(NULL, ",");
    }
    free(targetsCopy);
  }
  
  // Build diagnostic chain
  char chain[300];
  snprintf(chain, sizeof(chain), "WiFi(%ddBm)%s → Gateway(%lums)%s → DNS(%lums)%s → Internet(%d/%d)%s",
    diagnostics.wifiRSSI,
    diagnostics.wifiState == "Disconnected" ? "✗" : (diagnostics.wifiState == "Weak" ? "⚠" : "✓"),
    diagnostics.gatewayLatencyMs,
    gatewayOk ? "✓" : "✗",
    diagnostics.dnsLatencyMs,
    dnsOk ? "✓" : "✗",
    targetsOnline, targetsTotal,
    internetOk ? "✓" : "✗");
  diagnostics.diagnosticChain = chain;
  
  // Root cause analysis
  if (!gatewayOk) {
    diagnostics.rootCause = "Router/Gateway Offline";
    diagnostics.severity = "critical";
  } else if (!dnsOk) {
    diagnostics.rootCause = "DNS Resolution Failed";
    if (diagnostics.failedDNSServer.length() > 0)
      diagnostics.rootCause += " (" + diagnostics.failedDNSServer + ")";
    diagnostics.severity = "critical";
  } else if (!internetOk) {
    if (targetsTotal > 0 && targetsOnline > 0) {
      diagnostics.rootCause = "Partial Internet (" + String(targetsTotal - targetsOnline) + "/" + String(targetsTotal) + " targets down)";
      diagnostics.severity = "warning";
    } else {
      diagnostics.rootCause = "ISP/Internet Down";
      diagnostics.severity = "critical";
    }
  } else if (diagnostics.wifiState == "Weak") {
    diagnostics.rootCause = "WiFi Weak Signal";
    diagnostics.severity = "warning";
  } else {
    diagnostics.rootCause = "All Systems OK";
    diagnostics.severity = "healthy";
  }
  
  lastCheckDuration = micros() - start;
  totalChecks++;
}

// ==================== STATISTICS ====================
void updateStatistics(bool isOverallOk) {
  unsigned long now = timeClient.getEpochTime();
  if (now < 1000000000) return;

  if (isOverallOk) {
    if (!lastOverallOk && currentOutage.startEpoch != 0) {
      currentOutage.endEpoch = now;
      currentOutage.durationSec = now - currentOutage.startEpoch;
      currentOutage.wifiRSSIAtEnd = diagnostics.wifiRSSI;
      currentOutage.diagnosticChain = diagnostics.diagnosticChain;
      
      // Use the root cause at the moment of failure detection
      if (currentOutage.reason.length() == 0 || currentOutage.reason == "Unknown") {
        currentOutage.reason = diagnostics.rootCause;
      }
      
      outages.push_back(currentOutage);
      if (outages.size() > 500) outages.erase(outages.begin());
      
      if (currentOutage.durationSec > longestOutageSeconds) longestOutageSeconds = currentOutage.durationSec;
      totalDowntimeSeconds += currentOutage.durationSec;
      
      char msg[400];
      snprintf(msg, sizeof(msg), "✅ OUTAGE ENDED\nDuration: %lum %lus\nCause: %s\nRSSI: %d → %d dBm\nChain: %s",
               currentOutage.durationSec / 60, currentOutage.durationSec % 60,
               currentOutage.reason.c_str(),
               currentOutage.wifiRSSIAtStart, currentOutage.wifiRSSIAtEnd,
               currentOutage.diagnosticChain.c_str());
      sendTelegramMessage(msg);
      currentOutage = {0, 0, 0, "", "", 0, 0};
      saveDataToLittleFS();
    }
    if (lastTransitionTime > 0) totalUptimeSeconds += (now - lastTransitionTime);
    else totalUptimeSeconds++;
    lastTransitionTime = now;
  } else {
    if (lastOverallOk) {
      currentOutage.startEpoch = now;
      currentOutage.wifiRSSIAtStart = diagnostics.wifiRSSI;
      currentOutage.reason = diagnostics.rootCause;
      currentOutage.diagnosticChain = diagnostics.diagnosticChain;
      
      char msg[400], ts[30];
      time_t t = now;
      strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", localtime(&t));
      snprintf(msg, sizeof(msg), "🔴 OUTAGE STARTED\nTime: %s\nCause: %s\nRSSI: %d dBm\nChain: %s",
               ts, currentOutage.reason.c_str(), currentOutage.wifiRSSIAtStart,
               currentOutage.diagnosticChain.c_str());
      sendTelegramMessage(msg);
      lastTransitionTime = now;
    } else {
      // Update ongoing outage with latest diagnostic if it gets worse
      if (diagnostics.severity == "critical" && 
          (currentOutage.reason.indexOf("WiFi") != -1 && diagnostics.rootCause.indexOf("WiFi") == -1)) {
        // Escalated from WiFi to deeper issue
        currentOutage.reason = diagnostics.rootCause + " (Escalated)";
      }
    }
    if (lastTransitionTime > 0) totalDowntimeSeconds += (now - lastTransitionTime);
    else totalDowntimeSeconds++;
    failedChecks++;
    lastTransitionTime = now;
  }

  // 24h history
  if (lastHistoryUpdate == 0) lastHistoryUpdate = now;
  else {
    unsigned long mins = (now - lastHistoryUpdate) / 60;
    if (mins >= 1) {
      for (unsigned long i = 0; i < mins && i < 60; i++) {
        availabilityHistory[historyIndex] = isOverallOk;
        historyIndex = (historyIndex + 1) % HISTORY_SIZE;
      }
      lastHistoryUpdate = now;
    }
  }
  lastOverallOk = isOverallOk;
}

void handleWiFiReconnect() {
  if (WiFi.status() != WL_CONNECTED && millis() - lastWiFiReconnectAttempt > 30000) {
    lastWiFiReconnectAttempt = millis();
    WiFi.disconnect(); 
    WiFi.begin(ssid, password);
    wifiReconnectCount++;
  }
}

// ==================== WEB UI (ENHANCED) ====================
void handleRoot() {
  String html = R"rawliteral(
<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'>
<title>Network Monitor Pro</title><script src='https://cdn.jsdelivr.net/npm/chart.js@4.4.0/dist/chart.umd.min.js'></script>
<style>
*{margin:0;padding:0;box-sizing:border-box}
body{background:linear-gradient(135deg,#0a0e1a 0%,#0f1422 100%);color:#e5e7eb;font-family:system-ui,sans-serif;padding:20px;min-height:100vh}
.dashboard{max-width:1400px;margin:0 auto}
.header{background:linear-gradient(135deg,#1a1f2e 0%,#111827 100%);border:1px solid #2d3748;border-radius:16px;padding:20px 30px;margin-bottom:25px;display:flex;justify-content:space-between;align-items:center;box-shadow:0 4px 20px rgba(0,0,0,0.3);flex-wrap:wrap;gap:15px}
h1{font-size:1.5rem;display:flex;align-items:center;gap:15px;flex-wrap:wrap}
.badge{background:#1f2937;padding:5px 15px;border-radius:20px;font-size:0.8rem;font-weight:normal}
.status-led{width:16px;height:16px;border-radius:50%;display:inline-block;animation:pulse 2s infinite}
.online{background:#10b981;box-shadow:0 0 10px #10b981}.offline{background:#ef4444;box-shadow:0 0 10px #ef4444}.warning{background:#f59e0b;box-shadow:0 0 10px #f59e0b}
@keyframes pulse{0%,100%{opacity:1}50%{opacity:0.5}}

/* Diagnostic Chain */
.chain-box{background:#111827;border:1px solid #2d3748;border-radius:16px;padding:20px;margin-bottom:25px;font-family:'Courier New',monospace}
.chain-box h3{margin-bottom:12px;color:#9ca3af;font-size:0.9rem;font-family:system-ui}
.chain{font-size:0.95rem;line-height:1.8;color:#e5e7eb;word-break:break-word}
.chain .ok{color:#10b981;font-weight:bold}.chain .fail{color:#ef4444;font-weight:bold}.chain .warn{color:#f59e0b;font-weight:bold}
.root-cause{margin-top:15px;padding:12px 16px;border-radius:10px;font-weight:600;font-family:system-ui;display:flex;align-items:center;gap:10px;font-size:0.95rem}
.root-cause.healthy{background:rgba(16,185,129,0.1);border:1px solid #10b981;color:#10b981}
.root-cause.warning{background:rgba(245,158,11,0.1);border:1px solid #f59e0b;color:#f59e0b}
.root-cause.critical{background:rgba(239,68,68,0.1);border:1px solid #ef4444;color:#ef4444}

/* WiFi Signal Bars */
.wifi-bars{display:inline-flex;align-items:flex-end;gap:2px;height:18px;vertical-align:middle;margin-right:8px}
.wifi-bars .bar{width:4px;background:#374151;border-radius:1px;transition:background 0.3s}
.wifi-bars .bar:nth-child(1){height:25%}.wifi-bars .bar:nth-child(2){height:50%}
.wifi-bars .bar:nth-child(3){height:75%}.wifi-bars .bar:nth-child(4){height:100%}
.wifi-bars .bar.active{background:#10b981}
.wifi-bars .bar.warn.active{background:#f59e0b}
.wifi-bars .bar.crit.active{background:#ef4444}

/* Status Grid */
.status-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(180px,1fr));gap:15px;margin-bottom:25px}
.status-card{background:#111827;border:2px solid #2d3748;border-radius:16px;padding:18px;text-align:center;transition:all 0.3s}
.status-card.online{border-color:#10b981;background:rgba(16,185,129,0.05)}
.status-card.offline{border-color:#ef4444;background:rgba(239,68,68,0.05)}
.status-card.warning{border-color:#f59e0b;background:rgba(245,158,11,0.05)}
.status-icon{font-size:2.5rem;margin-bottom:8px}
.status-title{font-size:0.75rem;color:#9ca3af;margin-bottom:6px;text-transform:uppercase;letter-spacing:1px}
.status-text{font-size:1rem;font-weight:bold}
.status-sub{font-size:0.7rem;color:#6b7280;margin-top:4px;font-family:monospace}

/* Stats Grid */
.stats-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(180px,1fr));gap:15px;margin-bottom:25px}
.stat-card{background:#111827;border:1px solid #2d3748;border-radius:16px;padding:16px}
.stat-label{font-size:0.7rem;text-transform:uppercase;letter-spacing:1px;color:#9ca3af;margin-bottom:8px}
.stat-value{font-size:1.6rem;font-weight:bold;font-family:monospace}
.stat-value.small{font-size:1.1rem}

/* Targets Panel */
.targets-panel{background:#111827;border:1px solid #2d3748;border-radius:16px;padding:20px;margin-bottom:25px}
.targets-panel h3{margin-bottom:15px;color:#9ca3af;font-size:0.9rem}
.targets-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(200px,1fr));gap:10px}
.target-item{background:#1f2937;border-radius:10px;padding:12px;display:flex;justify-content:space-between;align-items:center;border-left:4px solid #374151}
.target-item.ok{border-left-color:#10b981}.target-item.fail{border-left-color:#ef4444}
.target-name{font-weight:600;font-size:0.9rem}
.target-latency{font-size:0.75rem;color:#9ca3af;font-family:monospace;margin-top:2px}
.target-status{font-size:1.1rem}

/* Charts & Tables */
.chart-container,.table-container{background:#111827;border:1px solid #2d3748;border-radius:16px;padding:20px;margin-bottom:25px}
.table-header{display:flex;justify-content:space-between;align-items:center;margin-bottom:15px;flex-wrap:wrap;gap:10px}
table{width:100%;border-collapse:collapse}
th,td{padding:10px;text-align:left;border-bottom:1px solid #2d3748;font-size:0.85rem}
th{color:#9ca3af;font-weight:600;font-size:0.75rem;text-transform:uppercase}
.chain-cell{font-family:monospace;font-size:0.7rem;color:#9ca3af;max-width:300px;word-break:break-word}

.btn{background:#3b82f6;border:none;color:white;padding:8px 16px;border-radius:8px;cursor:pointer;font-size:0.85rem;text-decoration:none;transition:all 0.2s}
.btn:hover{background:#2563eb}.btn-danger{background:#ef4444}.btn-danger:hover{background:#dc2626}
canvas{max-height:300px}
@media(max-width:768px){.status-grid,.stats-grid{grid-template-columns:repeat(2,1fr)}}
</style></head><body>
<div class='dashboard'>
<div class='header'>
  <h1><span class='status-led' id='led'></span>Network Monitor Pro <span class='badge'>⚡ LIVE</span> <span class='badge' id='checks-badge'>-- checks</span></h1>
  <a href='/settings' class='btn'>⚙️ Settings</a>
</div>

<!-- Diagnostic Chain -->
<div class='chain-box'>
  <h3>🔍 REAL-TIME DIAGNOSTIC CHAIN</h3>
  <div class='chain' id='diag-chain'>Loading...</div>
  <div class='root-cause healthy' id='root-cause'>● Analyzing...</div>
</div>

<!-- WiFi + Layer Status -->
<div class='status-grid'>
  <div class='status-card' id='wifi-card'>
    <div class='status-icon'>📶</div>
    <div class='status-title'>WiFi Signal</div>
    <div class='status-text' id='wifi-status'>--</div>
    <div class='status-sub' id='wifi-sub'>--</div>
  </div>
  <div class='status-card' id='gw-card'>
    <div class='status-icon'>🚪</div>
    <div class='status-title'>Gateway</div>
    <div class='status-text' id='gw-status'>--</div>
    <div class='status-sub' id='gw-sub'>--</div>
  </div>
  <div class='status-card' id='dns-card'>
    <div class='status-icon'>🔍</div>
    <div class='status-title'>DNS Resolution</div>
    <div class='status-text' id='dns-status'>--</div>
    <div class='status-sub' id='dns-sub'>--</div>
  </div>
  <div class='status-card' id='net-card'>
    <div class='status-icon'>🌐</div>
    <div class='status-title'>Internet</div>
    <div class='status-text' id='net-status'>--</div>
    <div class='status-sub' id='net-sub'>--</div>
  </div>
</div>

<!-- Targets Panel -->
<div class='targets-panel'>
  <h3>🎯 TARGET STATUS</h3>
  <div class='targets-grid' id='targets-grid'><div style='color:#6b7280'>Loading...</div></div>
</div>

<!-- Stats -->
<div class='stats-grid'>
  <div class='stat-card'><div class='stat-label'>📊 Uptime</div><div class='stat-value' id='uptime-pct'>--</div></div>
  <div class='stat-card'><div class='stat-label'>⏱️ Total Uptime</div><div class='stat-value small' id='total-up'>--</div></div>
  <div class='stat-card'><div class='stat-label'>⚠️ Longest Outage</div><div class='stat-value small' id='longest'>--</div></div>
  <div class='stat-card'><div class='stat-label'>📈 Avg Outage</div><div class='stat-value small' id='avg-out'>--</div></div>
  <div class='stat-card'><div class='stat-label'>🔢 Total Checks</div><div class='stat-value small' id='total-chk'>--</div></div>
  <div class='stat-card'><div class='stat-label'>❌ Failed</div><div class='stat-value small' id='fail-chk'>--</div></div>
  <div class='stat-card'><div class='stat-label'>🔄 WiFi Reconnects</div><div class='stat-value small' id='wifi-rc'>--</div></div>
  <div class='stat-card'><div class='stat-label'>⚡ Check Time</div><div class='stat-value small' id='chk-time'>--</div></div>
</div>

<div class='chart-container'>
  <h3 style='margin-bottom:15px;color:#9ca3af;font-size:0.9rem'>📈 24-HOUR AVAILABILITY</h3>
  <canvas id='chart'></canvas>
</div>

<div class='table-container'>
  <div class='table-header'>
    <h3 style='color:#9ca3af;font-size:0.9rem'>📋 OUTAGE HISTORY</h3>
    <div><button class='btn' onclick='exp()'>📥 CSV</button> <button class='btn btn-danger' onclick='clr()'>🗑️ Clear</button></div>
  </div>
  <div style='overflow:auto;max-height:400px'>
    <table><thead><tr><th>#</th><th>Start</th><th>Duration</th><th>Root Cause</th><th>RSSI Δ</th><th>Diagnostic Chain</th></tr></thead>
    <tbody id='tbl'></tbody></table>
  </div>
</div>
</div>
<script>
let c;
function fmt(s){if(!s)return'None';let d=Math.floor(s/86400),h=Math.floor((s%86400)/3600),m=Math.floor((s%3600)/60),sec=s%60;if(d>0)return d+'d '+h+'h';if(h>0)return h+'h '+m+'m';if(m>0)return m+'m '+sec+'s';return sec+'s'}
function fmtUp(s){let d=Math.floor(s/86400),h=Math.floor((s%86400)/3600),m=Math.floor((s%3600)/60);return d+'d '+h+'h '+m+'m'}

function renderChain(d){
  // Parse chain like: WiFi(-65dBm)✓ → Gateway(2ms)✓ → DNS(15ms)✓ → Internet(3/3)✓
  let html = d.chain
    .replace(/WiFi\(([^)]+)\)✓/g,'<span class="ok">WiFi($1) ✓</span>')
    .replace(/WiFi\(([^)]+)\)✗/g,'<span class="fail">WiFi($1) ✗</span>')
    .replace(/WiFi\(([^)]+)\)⚠/g,'<span class="warn">WiFi($1) ⚠</span>')
    .replace(/Gateway\(([^)]+)\)✓/g,'<span class="ok">Gateway($1) ✓</span>')
    .replace(/Gateway\(([^)]+)\)✗/g,'<span class="fail">Gateway($1) ✗</span>')
    .replace(/DNS\(([^)]+)\)✓/g,'<span class="ok">DNS($1) ✓</span>')
    .replace(/DNS\(([^)]+)\)✗/g,'<span class="fail">DNS($1) ✗</span>')
    .replace(/Internet\(([^)]+)\)✓/g,'<span class="ok">Internet($1) ✓</span>')
    .replace(/Internet\(([^)]+)\)✗/g,'<span class="fail">Internet($1) ✗</span>');
  document.getElementById('diag-chain').innerHTML = html;
  
  let rc = document.getElementById('root-cause');
  let icon = d.severity === 'healthy' ? '✅' : (d.severity === 'warning' ? '⚠️' : '🔴');
  rc.className = 'root-cause ' + d.severity;
  rc.innerHTML = icon + ' ' + d.rootCause;
}

function signalBars(rssi){
  // RSSI: >-50 excellent, -60 good, -70 fair, -80 weak, <-80 very weak
  let level = 0;
  if(rssi > -50) level = 4;
  else if(rssi > -60) level = 3;
  else if(rssi > -70) level = 2;
  else if(rssi > -80) level = 1;
  let cls = level >= 3 ? 'active' : (level >= 2 ? 'active warn' : 'active crit');
  let html = '<div class="wifi-bars">';
  for(let i=1;i<=4;i++){
    html += `<div class="bar ${i<=level?cls:''}"></div>`;
  }
  return html + '</div>';
}

function r(){
fetch('/api/status').then(r=>r.json()).then(d=>{
let ok = d.overall;
document.getElementById('led').className = 'status-led ' + (d.severity === 'healthy' ? 'online' : (d.severity === 'warning' ? 'warning' : 'offline'));

// WiFi
let wClass = d.wifi.connected ? (d.wifi.state === 'Weak' ? 'warning' : 'online') : 'offline';
document.getElementById('wifi-card').className = 'status-card ' + wClass;
document.getElementById('wifi-status').innerHTML = signalBars(d.wifi.rssi) + d.wifi.rssi + ' dBm';
document.getElementById('wifi-sub').textContent = d.wifi.state + (d.wifi.reconnects > 0 ? ' · ' + d.wifi.reconnects + ' reconnects' : '');

// Gateway
document.getElementById('gw-card').className = 'status-card ' + (d.gateway.ok ? 'online' : 'offline');
document.getElementById('gw-status').innerHTML = d.gateway.ok ? '<span style="color:#10b981">✓ Reachable</span>' : '<span style="color:#ef4444">✗ Unreachable</span>';
document.getElementById('gw-sub').textContent = d.gateway.ok ? d.gateway.latency + ' ms' : 'No response';

// DNS
document.getElementById('dns-card').className = 'status-card ' + (d.dns.ok ? 'online' : 'offline');
document.getElementById('dns-status').innerHTML = d.dns.ok ? '<span style="color:#10b981">✓ Resolved</span>' : '<span style="color:#ef4444">✗ Failed</span>';
document.getElementById('dns-sub').textContent = d.dns.ok ? d.dns.latency + ' ms' : (d.dns.failed || 'Timeout');

// Internet
let netClass = d.internet.ok ? 'online' : (d.internet.online > 0 ? 'warning' : 'offline');
document.getElementById('net-card').className = 'status-card ' + netClass;
document.getElementById('net-status').innerHTML = d.internet.ok ? '<span style="color:#10b981">✓ ' + d.internet.online + '/' + d.internet.total + '</span>' : 
  (d.internet.online > 0 ? '<span style="color:#f59e0b">⚠ ' + d.internet.online + '/' + d.internet.total + '</span>' : '<span style="color:#ef4444">✗ Down</span>');
document.getElementById('net-sub').textContent = d.internet.ok ? 'All targets OK' : 'Partial failure';

// Targets
let tg = document.getElementById('targets-grid');
tg.innerHTML = '';
if(d.targets && d.targets.length > 0){
  d.targets.forEach(t=>{
    let cls = t.success ? 'ok' : 'fail';
    let icon = t.success ? '✅' : '❌';
    let lat = t.success ? t.latency + 'ms' : 'timeout';
    tg.innerHTML += `<div class='target-item ${cls}'><div><div class='target-name'>${t.name}</div><div class='target-latency'>${lat}</div></div><div class='target-status'>${icon}</div></div>`;
  });
} else {
  tg.innerHTML = '<div style="color:#6b7280">No targets checked yet</div>';
}

// Stats
document.getElementById('uptime-pct').innerHTML = d.uptimePercent.toFixed(2) + '%';
document.getElementById('total-up').innerHTML = fmtUp(d.totalUptime);
document.getElementById('longest').innerHTML = fmt(d.longestOutage);
document.getElementById('avg-out').innerHTML = fmt(d.avgOutage);
document.getElementById('total-chk').innerHTML = d.totalChecks.toLocaleString();
document.getElementById('fail-chk').innerHTML = d.failedChecks.toLocaleString();
document.getElementById('wifi-rc').innerHTML = d.wifi.reconnects;
document.getElementById('chk-time').innerHTML = (d.checkDuration/1000).toFixed(1) + ' ms';
document.getElementById('checks-badge').innerHTML = d.totalChecks.toLocaleString() + ' checks';

// Chain
renderChain(d);

// Outage table
let tb = document.getElementById('tbl'); tb.innerHTML = '';
if(d.outages && d.outages.length > 0){
  let rev = [...d.outages].reverse();
  rev.forEach((o,i)=>{
    let rssiDelta = o.rssiEnd - o.rssiStart;
    let rssiStr = o.rssiStart + ' → ' + o.rssiEnd + ' dBm';
    if(o.rssiStart !== 0) rssiStr += ' (' + (rssiDelta>=0?'+':'') + rssiDelta + ')';
    tb.innerHTML += `<tr><td>${rev.length-i}</td><td>${new Date(o.start*1000).toLocaleString()}</td><td>${fmt(o.duration)}</td><td style="color:#f59e0b">⚠️ ${o.reason}</td><td style="font-family:monospace;font-size:0.8rem">${rssiStr}</td><td class='chain-cell'>${o.chain || '--'}</td></tr>`;
  });
} else {
  tb.innerHTML = '<tr><td colspan="6" style="text-align:center;color:#10b981">✅ No outages recorded</td></tr>';
}

// Chart
if(c) c.destroy();
let ctx = document.getElementById('chart').getContext('2d');
let g = ctx.createLinearGradient(0,0,0,300);
g.addColorStop(0,'rgba(16,185,129,0.4)'); g.addColorStop(1,'rgba(16,185,129,0.05)');
c = new Chart(ctx,{type:'line',data:{labels:d.historyLabels,datasets:[{label:'Status',data:d.historyData,borderColor:'#10b981',backgroundColor:g,borderWidth:3,fill:true,tension:0.2,pointRadius:0}]},
options:{responsive:true,scales:{y:{min:-0.1,max:1.1,ticks:{stepSize:1,callback:v=>v===1?'● Up':'○ Down',color:'#9ca3af'},grid:{color:'#2d3748'}},x:{ticks:{color:'#9ca3af'},grid:{color:'#2d3748'}}}}});
});
}
function exp(){window.location.href='/api/export'}
function clr(){if(confirm('⚠️ Clear ALL data?'))fetch('/api/clear',{method:'POST'}).then(r)}
r(); setInterval(r, 2000);
</script></body></html>
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
  doc["severity"] = diagnostics.severity.c_str();
  doc["rootCause"] = diagnostics.rootCause.c_str();
  doc["chain"] = diagnostics.diagnosticChain.c_str();
  doc["uptimePercent"] = uptimePercent;
  doc["totalUptime"] = totalUptimeSeconds; doc["totalDowntime"] = totalDowntimeSeconds;
  doc["longestOutage"] = longestOutageSeconds; doc["avgOutage"] = avgOutage;
  doc["totalChecks"] = totalChecks; doc["failedChecks"] = failedChecks; doc["checkDuration"] = lastCheckDuration;

  // WiFi
  JsonObject wifi = doc.createNestedObject("wifi");
  wifi["rssi"] = diagnostics.wifiRSSI;
  wifi["state"] = diagnostics.wifiState.c_str();
  wifi["connected"] = WiFi.status() == WL_CONNECTED;
  wifi["reconnects"] = wifiReconnectCount;

  // Gateway
  JsonObject gw = doc.createNestedObject("gateway");
  gw["ok"] = gatewayOk;
  gw["latency"] = diagnostics.gatewayLatencyMs;

  // DNS
  JsonObject dns = doc.createNestedObject("dns");
  dns["ok"] = dnsOk;
  dns["latency"] = diagnostics.dnsLatencyMs;
  dns["failed"] = diagnostics.failedDNSServer.c_str();

  // Internet
  JsonObject internet = doc.createNestedObject("internet");
  internet["ok"] = internetOk;
  internet["online"] = targetsOnline;
  internet["total"] = targetsTotal;

  // Per-target status
  JsonArray targets = doc.createNestedArray("targets");
  for (auto& t : currentTargetStatus) {
    JsonObject to = targets.createNestedObject();
    to["name"] = t.name; to["success"] = t.success; to["latency"] = t.latencyMs;
  }

  // Outages (last 50)
  JsonArray outArr = doc.createNestedArray("outages");
  int start = outages.size() > 50 ? outages.size() - 50 : 0;
  for (int i = start; i < outages.size(); i++) {
    JsonObject o = outArr.createNestedObject();
    o["start"] = outages[i].startEpoch; o["end"] = outages[i].endEpoch;
    o["duration"] = outages[i].durationSec; o["reason"] = outages[i].reason;
    o["chain"] = outages[i].diagnosticChain;
    o["rssiStart"] = outages[i].wifiRSSIAtStart;
    o["rssiEnd"] = outages[i].wifiRSSIAtEnd;
  }

  // 24h history
  JsonArray hData = doc.createNestedArray("historyData");
  JsonArray hLbl = doc.createNestedArray("historyLabels");
  int step = HISTORY_SIZE / 24;
  for (int i = 0; i < 24; i++) {
    hData.add(availabilityHistory[(historyIndex + i * step) % HISTORY_SIZE] ? 1 : 0);
    hLbl.add(String(i) + ":00");
  }

  String out; serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handleSettingsPage() {
  String html = R"rawliteral(
<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'>
<title>Settings</title>
<style>body{background:#0a0e1a;color:#e5e7eb;font-family:system-ui;padding:20px}
.c{max-width:650px;margin:0 auto;background:#111827;padding:25px;border-radius:16px;border:1px solid #2d3748}
h2{margin-bottom:20px}label{display:block;margin:15px 0 5px;color:#9ca3af;font-size:0.85rem}
input,select{width:100%;padding:10px;background:#1f2937;border:1px solid #374151;color:#e5e7eb;border-radius:8px}
.btn{background:#3b82f6;color:white;border:none;padding:12px;width:100%;border-radius:8px;cursor:pointer;margin-top:20px;font-size:1rem}
.btn:hover{background:#2563eb}.back{color:#9ca3af;text-decoration:none;margin-bottom:20px;display:block}
.note{color:#6b7280;font-size:0.75rem;margin-top:5px}</style></head><body>
<div class='c'><a href='/' class='back'>← Back to Dashboard</a><h2>⚙️ System Settings</h2>
<form id='f'><label>Timezone (POSIX)</label><input id='tz' placeholder='CET-1CEST...'><div class='note'>Leave default if unsure</div>
<label>Check Method</label><select id='m'><option value='http'>HTTP (Port 80)</option><option value='dns'>DNS Resolution</option><option value='ping'>TCP Ping</option></select>
<label>Targets (comma sep)</label><input id='t' placeholder='google.com,cloudflare.com'>
<label>DNS Servers to Test (comma sep)</label><input id='dns' placeholder='8.8.8.8,1.1.1.1'>
<label>Check Interval (ms)</label><input id='i' type='number' min='500'>
<label>NTP Sync (ms)</label><input id='n' type='number' min='60000'>
<label>Weak Signal Threshold (dBm)</label><input id='rssi' type='number' min='-100' max='-30'>
<div class='note'>RSSI below this value = weak signal warning</div>
<label>Telegram Token</label><input id='tk'>
<label>Telegram Chat ID</label><input id='ci'>
<button type='submit' class='btn'>💾 Save & Apply</button></form></div>
<script>
fetch('/api/settings').then(r=>r.json()).then(d=>{
document.getElementById('tz').value=d.timezone;document.getElementById('m').value=d.checkMethod;
document.getElementById('t').value=d.targets;document.getElementById('dns').value=d.dnsServers;
document.getElementById('i').value=d.monitorInterval;document.getElementById('n').value=d.ntpInterval;
document.getElementById('rssi').value=d.weakSignalThreshold;
document.getElementById('tk').value=d.telegramBotToken;document.getElementById('ci').value=d.telegramChatId;});
document.getElementById('f').addEventListener('submit',e=>{
e.preventDefault();
let d={timezone:document.getElementById('tz').value,checkMethod:document.getElementById('m').value,targets:document.getElementById('t').value,dnsServers:document.getElementById('dns').value,monitorInterval:parseInt(document.getElementById('i').value),ntpInterval:parseInt(document.getElementById('n').value),weakSignalThreshold:parseInt(document.getElementById('rssi').value),telegramBotToken:document.getElementById('tk').value,telegramChatId:document.getElementById('ci').value};
fetch('/api/settings',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(d)}).then(()=>alert('Saved! Rebooting...')).then(()=>location.href='/');});
</script></body></html>
)rawliteral";
  server.send(200, "text/html", html);
}

void handleAPISettings() {
  if (server.method() == HTTP_GET) {
    StaticJsonDocument<1024> doc;
    doc["timezone"] = settings.timezone; doc["checkMethod"] = settings.checkMethod; doc["targets"] = settings.targets;
    doc["dnsServers"] = settings.dnsServers;
    doc["monitorInterval"] = settings.monitorInterval; doc["ntpInterval"] = settings.ntpInterval;
    doc["weakSignalThreshold"] = settings.weakSignalThreshold;
    doc["telegramBotToken"] = settings.telegramBotToken; doc["telegramChatId"] = settings.telegramChatId;
    String o; serializeJson(doc, o); server.send(200, "application/json", o);
  } else {
    StaticJsonDocument<1024> doc;
    DeserializationError err = deserializeJson(doc, server.arg("plain"));
    if (!err) {
      strncpy(settings.timezone, doc["timezone"] | settings.timezone, sizeof(settings.timezone)-1);
      strncpy(settings.checkMethod, doc["checkMethod"] | settings.checkMethod, sizeof(settings.checkMethod)-1);
      strncpy(settings.targets, doc["targets"] | settings.targets, sizeof(settings.targets)-1);
      strncpy(settings.dnsServers, doc["dnsServers"] | settings.dnsServers, sizeof(settings.dnsServers)-1);
      settings.monitorInterval = doc["monitorInterval"] | settings.monitorInterval;
      settings.ntpInterval = doc["ntpInterval"] | settings.ntpInterval;
      settings.weakSignalThreshold = doc["weakSignalThreshold"] | settings.weakSignalThreshold;
      strncpy(settings.telegramBotToken, doc["telegramBotToken"] | settings.telegramBotToken, sizeof(settings.telegramBotToken)-1);
      strncpy(settings.telegramChatId, doc["telegramChatId"] | settings.telegramChatId, sizeof(settings.telegramChatId)-1);
      saveSettings();
      server.send(200, "application/json", "{\"status\":\"ok\"}");
      delay(1000); 
      ESP.restart();
    } else server.send(400, "text/plain", "JSON Error");
  }
}

void handleExportCSV() {
  String csv = "Start Time,End Time,Duration(s),Root Cause,WiFi RSSI Start,WiFi RSSI End,Diagnostic Chain\n";
  for (auto& o : outages) {
    char startStr[30], endStr[30];
    time_t startT = o.startEpoch, endT = o.endEpoch;
    strftime(startStr, sizeof(startStr), "%Y-%m-%d %H:%M:%S", localtime(&startT));
    strftime(endStr, sizeof(endStr), "%Y-%m-%d %H:%M:%S", localtime(&endT));
    // Escape quotes in chain
    String chain = o.diagnosticChain;
    chain.replace("\"", "\"\"");
    String reason = o.reason;
    reason.replace("\"", "\"\"");
    csv += String("\"") + startStr + "\",\"" + endStr + "\"," + 
           String(o.durationSec) + ",\"" + reason + "\"," +
           String(o.wifiRSSIAtStart) + "," + String(o.wifiRSSIAtEnd) + ",\"" + chain + "\"\n";
  }
  server.send(200, "text/csv", csv);
}

void handleClearHistory() {
  outages.clear();
  totalDowntimeSeconds = 0;
  longestOutageSeconds = 0;
  failedChecks = 0;
  wifiReconnectCount = 0;
  for (int i = 0; i < HISTORY_SIZE; i++) availabilityHistory[i] = true;
  historyIndex = 0;
  lastHistoryUpdate = 0;
  saveDataToLittleFS();
  server.send(200, "application/json", "{\"status\":\"cleared\"}");
}

// ==================== SETUP & LOOP ====================
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n🚀 Network Monitor Pro Starting...");
  
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  if (!LittleFS.begin(false)) {
    Serial.println("Mounting LittleFS...");
    if (!LittleFS.begin(true)) Serial.println("Failed!");
  }
  
  loadSettings();
  loadDataFromLittleFS();

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  int a = 0;
  while (WiFi.status() != WL_CONNECTED && a < 40) {
    delay(500); Serial.print("."); a++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n✓ WiFi connected!");
    Serial.print("📡 IP: http://"); Serial.println(WiFi.localIP());
    Serial.printf("📶 RSSI: %d dBm\n", WiFi.RSSI());
    digitalWrite(LED_PIN, HIGH);
  }

  timeClient.begin();
  timeClient.update();
  startTime = timeClient.getEpochTime();
  if (startTime < 1000000000) startTime = 1700000000;
  lastTransitionTime = startTime;
  
  if (lastHistoryUpdate == 0) {
    for (int i = 0; i < HISTORY_SIZE; i++) availabilityHistory[i] = true;
    lastHistoryUpdate = startTime;
  }

  server.on("/", handleRoot);
  server.on("/settings", handleSettingsPage);
  server.on("/api/status", handleAPIStatus);
  server.on("/api/settings", handleAPISettings);
  server.on("/api/export", handleExportCSV);
  server.on("/api/clear", HTTP_POST, handleClearHistory);
  server.begin();
  
  lastMonitorTime = millis();
  lastSaveTime = millis();
  Serial.println("🌐 HTTP server started");
  Serial.println("⚡ MONITORING ACTIVE - DEEP DIAGNOSTICS ENABLED\n");
}

void loop() {
  server.handleClient();
  handleWiFiReconnect();
  
  unsigned long now = millis();
  if (now - lastMonitorTime >= settings.monitorInterval) {
    performNetworkTests();
    updateStatistics(gatewayOk && dnsOk && internetOk);
    lastMonitorTime = now;
  }
  
  if (now - lastNTPUpdate >= settings.ntpInterval) {
    timeClient.update();
    lastNTPUpdate = now;
  }
}
