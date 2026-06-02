#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <WiFiClientSecure.h> // 🔧 FIX: Required for Telegram HTTPS
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <time.h>

// ==================== CONFIGURATION & SETTINGS ====================
const char* ssid = "🪔";
const char* password = "pollob12";
const int LED_PIN = 8;

// Dynamic Settings Structure (Saved to LittleFS)
struct SystemSettings {
  char timezone[64] = "CET-1CEST,M3.5.0,M10.5.0/3";
  char checkMethod[16] = "http";
  char targets[256] = "google.com,microsoft.com,cloudflare.com";
  unsigned long monitorInterval = 1000;
  unsigned long ntpInterval = 3600000; // 1 Hour
  char telegramBotToken[128] = "";
  char telegramChatId[32] = "";
} settings;

// ==================== GLOBAL STATE ====================
WebServer server(80);
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 0, 60000); // Offset handled by TZ env

bool gatewayOk = false, dnsOk = false, internetOk = false;
bool lastOverallOk = true;
unsigned long lastMonitorTime = 0, lastSaveTime = 0, lastNTPUpdate = 0;
unsigned long startTime = 0, totalChecks = 0, failedChecks = 0;
unsigned long totalUptimeSeconds = 0, totalDowntimeSeconds = 0, longestOutageSeconds = 0;
unsigned long lastTransitionTime = 0, lastCheckDuration = 0;
unsigned long lastWiFiReconnectAttempt = 0;

struct OutageEvent {
  unsigned long startEpoch, endEpoch, durationSec;
  String reason;
};
std::vector<OutageEvent> outages;
OutageEvent currentOutage = {0, 0, 0, ""};

const int HISTORY_SIZE = 1440; // 24 hours (1 per minute)
bool availabilityHistory[HISTORY_SIZE];
int historyIndex = 0;
unsigned long lastHistoryUpdate = 0;

// ==================== TELEGRAM NOTIFICATION ====================
void sendTelegramMessage(String message) {
  if (strlen(settings.telegramBotToken) == 0) return;
  
  WiFiClientSecure client;
  client.setInsecure(); // Skip cert verification
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
  doc["monitorInterval"] = settings.monitorInterval;
  doc["ntpInterval"] = settings.ntpInterval;
  doc["telegramBotToken"] = settings.telegramBotToken;
  doc["telegramChatId"] = settings.telegramChatId;
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
  settings.monitorInterval = doc["monitorInterval"] | settings.monitorInterval;
  settings.ntpInterval = doc["ntpInterval"] | settings.ntpInterval;
  strncpy(settings.telegramBotToken, doc["telegramBotToken"] | settings.telegramBotToken, sizeof(settings.telegramBotToken)-1);
  strncpy(settings.telegramChatId, doc["telegramChatId"] | settings.telegramChatId, sizeof(settings.telegramChatId)-1);
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
  
  JsonArray outagesArray = doc.createNestedArray("outages");
  for (auto& o : outages) {
    JsonObject obj = outagesArray.createNestedObject();
    obj["start"] = o.startEpoch; obj["end"] = o.endEpoch;
    obj["duration"] = o.durationSec; obj["reason"] = o.reason;
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
  
  JsonArray outagesArray = doc["outages"];
  outages.clear();
  for (JsonObject obj : outagesArray) {
    OutageEvent ev;
    ev.startEpoch = obj["start"]; ev.endEpoch = obj["end"];
    ev.durationSec = obj["duration"]; ev.reason = obj["reason"] | "Unknown";
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

// ==================== NETWORK TESTS ====================
bool checkTarget(const char* target, const char* method) {
  WiFiClient client; 
  client.setTimeout(800);
  if (strcmp(method, "http") == 0) return client.connect(target, 80);
  if (strcmp(method, "ping") == 0) return client.connect(target, 53) || client.connect(target, 443);
  return false; 
}

bool checkGateway() {
  IPAddress gw = WiFi.gatewayIP();
  if (gw != INADDR_NONE && gw != IPAddress(0,0,0,0)) {
    WiFiClient c; c.setTimeout(500);
    if (c.connect(gw, 80)) return true;
  }
  return false;
}

bool checkDNS() {
  IPAddress r; 
  return (WiFi.hostByName("8.8.8.8", r) == 1);
}

bool checkInternet() {
  char* targetsCopy = strdup(settings.targets);
  char* token = strtok(targetsCopy, ",");
  bool anyOk = false;
  while (token != NULL) {
    while (*token == ' ') token++;
    if (checkTarget(token, settings.checkMethod)) { anyOk = true; break; }
    token = strtok(NULL, ",");
  }
  free(targetsCopy);
  return anyOk;
}

void performNetworkTests() {
  unsigned long start = micros();
  gatewayOk = checkGateway();
  dnsOk = gatewayOk ? checkDNS() : false;
  internetOk = (gatewayOk && dnsOk) ? checkInternet() : false;
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
      if (!gatewayOk) currentOutage.reason = "Gateway Failed";
      else if (!dnsOk) currentOutage.reason = "DNS Failed";
      else currentOutage.reason = "Internet Failed";
      
      outages.push_back(currentOutage);
      if (outages.size() > 500) outages.erase(outages.begin()); // 🔧 FIX: Prevent Memory Exhaustion
      
      if (currentOutage.durationSec > longestOutageSeconds) longestOutageSeconds = currentOutage.durationSec;
      totalDowntimeSeconds += currentOutage.durationSec;
      
      char msg[200];
      snprintf(msg, sizeof(msg), "🔴 OUTAGE ENDED\nDuration: %lum %lus\nReason: %s",
               currentOutage.durationSec / 60, currentOutage.durationSec % 60, currentOutage.reason.c_str());
      sendTelegramMessage(msg);
      currentOutage = {0, 0, 0, ""};
      saveDataToLittleFS();
    }
    if (lastTransitionTime > 0) totalUptimeSeconds += (now - lastTransitionTime);
    else totalUptimeSeconds++;
    lastTransitionTime = now;
  } else {
    if (lastOverallOk) {
      currentOutage.startEpoch = now;
      char msg[100], ts[30];
      time_t t = now;
      strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", localtime(&t));
      snprintf(msg, sizeof(msg), "⚠️ OUTAGE STARTED\nTime: %s", ts);
      sendTelegramMessage(msg);
      lastTransitionTime = now;
    }
    if (lastTransitionTime > 0) totalDowntimeSeconds += (now - lastTransitionTime);
    else totalDowntimeSeconds++;
    failedChecks++;
    lastTransitionTime = now;
  }

  // Update 24h History
  if (lastHistoryUpdate == 0) {
    lastHistoryUpdate = now;
  } else {
    unsigned long minutesPassed = (now - lastHistoryUpdate) / 60;
    if (minutesPassed >= 1) {
      for (unsigned long i = 0; i < minutesPassed && i < 60; i++) {
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
  }
}

// ==================== WEB HANDLERS ====================
void handleRoot() {
  String html = R"rawliteral(
<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'>
<title>Network Monitor</title><script src='https://cdn.jsdelivr.net/npm/chart.js@4.4.0/dist/chart.umd.min.js'></script>
<style>
*{margin:0;padding:0;box-sizing:border-box}body{background:linear-gradient(135deg,#0a0e1a 0%,#0f1422 100%);color:#e5e7eb;font-family:system-ui,sans-serif;padding:20px;min-height:100vh}
.dashboard{max-width:1400px;margin:0 auto}.header{background:linear-gradient(135deg,#1a1f2e 0%,#111827 100%);border:1px solid #2d3748;border-radius:16px;padding:20px 30px;margin-bottom:25px;display:flex;justify-content:space-between;align-items:center;box-shadow:0 4px 20px rgba(0,0,0,0.3)}
h1{font-size:1.5rem;display:flex;align-items:center;gap:15px;flex-wrap:wrap}.badge{background:#1f2937;padding:5px 15px;border-radius:20px;font-size:0.8rem;font-weight:normal}
.status-led{width:16px;height:16px;border-radius:50%;display:inline-block;animation:pulse 2s infinite}.online{background:#10b981;box-shadow:0 0 10px #10b981}.offline{background:#ef4444;box-shadow:0 0 10px #ef4444}
@keyframes pulse{0%,100%{opacity:1}50%{opacity:0.5}}
.stats-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(200px,1fr));gap:20px;margin-bottom:25px}
.stat-card{background:#111827;border:1px solid #2d3748;border-radius:16px;padding:20px;transition:transform 0.2s}.stat-card:hover{transform:translateY(-2px)}
.stat-label{font-size:0.75rem;text-transform:uppercase;letter-spacing:1px;color:#9ca3af;margin-bottom:10px}.stat-value{font-size:1.8rem;font-weight:bold;font-family:monospace}.stat-value.small{font-size:1.2rem}
.status-grid{display:grid;grid-template-columns:repeat(4,1fr);gap:20px;margin-bottom:25px}
.status-card{background:#111827;border:2px solid #2d3748;border-radius:16px;padding:20px;text-align:center;transition:all 0.3s}.status-card.online{border-color:#10b981;background:rgba(16,185,129,0.05)}.status-card.offline{border-color:#ef4444;background:rgba(239,68,68,0.05)}
.status-icon{font-size:3rem;margin-bottom:10px}.status-title{font-size:0.9rem;color:#9ca3af;margin-bottom:8px}.status-text{font-size:1.2rem;font-weight:bold}
.chart-container,.table-container{background:#111827;border:1px solid #2d3748;border-radius:16px;padding:20px;margin-bottom:25px}
.table-header{display:flex;justify-content:space-between;align-items:center;margin-bottom:15px;flex-wrap:wrap;gap:10px}
table{width:100%;border-collapse:collapse}th,td{padding:12px;text-align:left;border-bottom:1px solid #2d3748}th{color:#9ca3af;font-weight:600;font-size:0.8rem}
.btn{background:#3b82f6;border:none;color:white;padding:8px 16px;border-radius:8px;cursor:pointer;font-size:0.85rem;transition:all 0.2s;text-decoration:none}.btn:hover{background:#2563eb}
.btn-danger{background:#ef4444}.btn-danger:hover{background:#dc2626}
canvas{max-height:300px}
@media(max-width:768px){.status-grid,.stats-grid{grid-template-columns:repeat(2,1fr)}}
</style></head><body>
<div class='dashboard'>
<div class='header'>
  <h1><span class='status-led' id='led-indicator'></span>ESP32 Monitor <span class='badge'>⚡ LIVE</span> <span class='badge' id='check-counter'>-- checks</span></h1>
  <a href='/settings' class='btn'>⚙️ Settings</a>
</div>
<div class='status-grid'>
<div class='status-card' id='overall-card'><div class='status-icon'>🌐</div><div class='status-title'>OVERALL</div><div class='status-text' id='overall-status'>--</div></div>
<div class='status-card' id='gateway-card'><div class='status-icon'>🚪</div><div class='status-title'>GATEWAY</div><div class='status-text' id='gateway-status'>--</div></div>
<div class='status-card' id='dns-card'><div class='status-icon'>🔍</div><div class='status-title'>DNS</div><div class='status-text' id='dns-status'>--</div></div>
<div class='status-card' id='internet-card'><div class='status-icon'>🌍</div><div class='status-title'>INTERNET</div><div class='status-text' id='internet-status'>--</div></div>
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
<div class='chart-container'><h3 style='margin-bottom:15px;color:#9ca3af'>📈 24-HOUR HISTORY</h3><canvas id='chart'></canvas></div>
<div class='table-container'>
<div class='table-header'><h3>📋 OUTAGE HISTORY</h3>
<div><button class='btn' onclick='exp()'>📥 CSV</button> <button class='btn btn-danger' onclick='clr()'>🗑️ Clear</button></div></div>
<div style='overflow:auto;max-height:400px'><table><thead><tr><th>#</th><th>Start</th><th>End</th><th>Duration</th><th>Reason</th></tr></thead><tbody id='tbl'></tbody></table></div>
</div>
</div>
<script>
let c;
function fmt(s){if(!s)return'None';let d=Math.floor(s/86400),h=Math.floor((s%86400)/3600),m=Math.floor((s%3600)/60),sec=s%60;if(d>0)return d+'d '+h+'h';if(h>0)return h+'h '+m+'m';if(m>0)return m+'m '+sec+'s';return sec+'s'}
function fmtUp(s){let d=Math.floor(s/86400),h=Math.floor((s%86400)/3600),m=Math.floor((s%3600)/60);return d+'d '+h+'h '+m+'m'}
function r(){
fetch('/api/status').then(r=>r.json()).then(d=>{
let ok=d.overall;
document.getElementById('led-indicator').className='status-led '+(ok?'online':'offline');
document.getElementById('overall-status').innerHTML=ok?'<span style="color:#10b981">✓ ONLINE</span>':'<span style="color:#ef4444">✗ OFFLINE</span>';
document.getElementById('gateway-status').innerHTML=d.gateway?'<span style="color:#10b981">✓ OK</span>':'<span style="color:#ef4444">✗ FAIL</span>';
document.getElementById('dns-status').innerHTML=d.dns?'<span style="color:#10b981">✓ OK</span>':'<span style="color:#ef4444">✗ FAIL</span>';
document.getElementById('internet-status').innerHTML=d.internet?'<span style="color:#10b981">✓ OK</span>':'<span style="color:#ef4444">✗ FAIL</span>';
['overall','gateway','dns','internet'].forEach(k=>{document.getElementById(k+'-card').className='status-card '+(d[k]?'online':'offline')});
document.getElementById('uptime-percent').innerHTML=d.uptimePercent.toFixed(2)+'%';
document.getElementById('total-uptime').innerHTML=fmtUp(d.totalUptime);
document.getElementById('longest-outage').innerHTML=fmt(d.longestOutage);
document.getElementById('avg-outage').innerHTML=fmt(d.avgOutage);
document.getElementById('total-checks').innerHTML=d.totalChecks.toLocaleString();
document.getElementById('failed-checks').innerHTML=d.failedChecks.toLocaleString();
let avail = d.totalChecks > 0 ? ((d.totalChecks - d.failedChecks) / d.totalChecks * 100).toFixed(2) : '100.00';
document.getElementById('availability').innerHTML=avail+'%';
document.getElementById('check-time').innerHTML=(d.checkDuration/1000).toFixed(1)+' ms';
document.getElementById('check-counter').innerHTML=d.totalChecks.toLocaleString()+' checks';
let tb=document.getElementById('tbl');tb.innerHTML='';
if(d.outages&&d.outages.length>0){let rev=[...d.outages].reverse();rev.forEach((o,i)=>{tb.innerHTML+=`<tr><td>${rev.length-i}</td><td>${new Date(o.start*1000).toLocaleString()}</td><td>${new Date(o.end*1000).toLocaleString()}</td><td>${fmt(o.duration)}</td><td><span style="color:#f59e0b">⚠️ ${o.reason}</span></td></tr>`})}else{tb.innerHTML='<tr><td colspan=5 style="text-align:center">✅ No outages recorded</td></tr>'}
if(c)c.destroy();let ctx=document.getElementById('chart').getContext('2d');let g=ctx.createLinearGradient(0,0,0,300);g.addColorStop(0,'rgba(16,185,129,0.4)');g.addColorStop(1,'rgba(16,185,129,0.05)');
c=new Chart(ctx,{type:'line',data:{labels:d.historyLabels,datasets:[{label:'Status',data:d.historyData,borderColor:'#10b981',backgroundColor:g,borderWidth:3,fill:true,tension:0.2,pointRadius:0}]},options:{responsive:true,scales:{y:{min:-0.1,max:1.1,ticks:{stepSize:1,callback:v=>v===1?'● Up':'○ Down',color:'#9ca3af'},grid:{color:'#2d3748'}},x:{ticks:{color:'#9ca3af'},grid:{color:'#2d3748'}}}}});
});}
function exp(){window.location.href='/api/export'}
function clr(){if(confirm('⚠️ Clear ALL data?'))fetch('/api/clear',{method:'POST'}).then(r)}
r();setInterval(r,2000);
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
  doc["gateway"] = gatewayOk; doc["dns"] = dnsOk; doc["internet"] = internetOk;
  doc["uptimePercent"] = uptimePercent;
  doc["totalUptime"] = totalUptimeSeconds; doc["totalDowntime"] = totalDowntimeSeconds;
  doc["longestOutage"] = longestOutageSeconds; doc["avgOutage"] = avgOutage;
  doc["totalChecks"] = totalChecks; doc["failedChecks"] = failedChecks; doc["checkDuration"] = lastCheckDuration;

  JsonArray outArr = doc.createNestedArray("outages");
  int start = outages.size() > 50 ? outages.size() - 50 : 0;
  for (int i = start; i < outages.size(); i++) {
    JsonObject o = outArr.createNestedObject();
    o["start"] = outages[i].startEpoch; o["end"] = outages[i].endEpoch;
    o["duration"] = outages[i].durationSec; o["reason"] = outages[i].reason;
  }

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
.c{max-width:600px;margin:0 auto;background:#111827;padding:25px;border-radius:16px;border:1px solid #2d3748}
h2{margin-bottom:20px}label{display:block;margin:15px 0 5px;color:#9ca3af;font-size:0.85rem}
input,select{width:100%;padding:10px;background:#1f2937;border:1px solid #374151;color:#e5e7eb;border-radius:8px}
.btn{background:#3b82f6;color:white;border:none;padding:12px;width:100%;border-radius:8px;cursor:pointer;margin-top:20px;font-size:1rem}
.btn:hover{background:#2563eb}.back{color:#9ca3af;text-decoration:none;margin-bottom:20px;display:block}
.note{color:#6b7280;font-size:0.75rem;margin-top:5px}</style></head><body>
<div class='c'><a href='/' class='back'>← Back to Dashboard</a><h2>⚙️ System Settings</h2>
<form id='f'><label>Timezone (POSIX)</label><input id='tz' placeholder='CET-1CEST...'><div class='note'>Leave default if unsure</div>
<label>Check Method</label><select id='m'><option value='http'>HTTP (Port 80)</option><option value='dns'>DNS Resolution</option><option value='ping'>TCP Ping</option></select>
<label>Targets (comma sep)</label><input id='t' placeholder='google.com,cloudflare.com'>
<label>Check Interval (ms)</label><input id='i' type='number' min='500'>
<label>NTP Sync (ms)</label><input id='n' type='number' min='60000'>
<label>Telegram Token</label><input id='tk'>
<label>Telegram Chat ID</label><input id='ci'>
<button type='submit' class='btn'>💾 Save & Apply</button></form></div>
<script>
fetch('/api/settings').then(r=>r.json()).then(d=>{
document.getElementById('tz').value=d.timezone;document.getElementById('m').value=d.checkMethod;
document.getElementById('t').value=d.targets;document.getElementById('i').value=d.monitorInterval;
document.getElementById('n').value=d.ntpInterval;document.getElementById('tk').value=d.telegramBotToken;
document.getElementById('ci').value=d.telegramChatId;});
document.getElementById('f').addEventListener('submit',e=>{
e.preventDefault();
let d={timezone:document.getElementById('tz').value,checkMethod:document.getElementById('m').value,targets:document.getElementById('t').value,monitorInterval:parseInt(document.getElementById('i').value),ntpInterval:parseInt(document.getElementById('n').value),telegramBotToken:document.getElementById('tk').value,telegramChatId:document.getElementById('ci').value};
fetch('/api/settings',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(d)}).then(()=>alert('Saved! Rebooting...')).then(()=>location.href='/');});
</script></body></html>
)rawliteral";
  server.send(200, "text/html", html);
}

void handleAPISettings() {
  if (server.method() == HTTP_GET) {
    StaticJsonDocument<1024> doc;
    doc["timezone"] = settings.timezone; doc["checkMethod"] = settings.checkMethod; doc["targets"] = settings.targets;
    doc["monitorInterval"] = settings.monitorInterval; doc["ntpInterval"] = settings.ntpInterval;
    doc["telegramBotToken"] = settings.telegramBotToken; doc["telegramChatId"] = settings.telegramChatId;
    String o; serializeJson(doc, o); server.send(200, "application/json", o);
  } else {
    StaticJsonDocument<1024> doc;
    DeserializationError err = deserializeJson(doc, server.arg("plain"));
    if (!err) {
      strncpy(settings.timezone, doc["timezone"] | settings.timezone, sizeof(settings.timezone)-1);
      strncpy(settings.checkMethod, doc["checkMethod"] | settings.checkMethod, sizeof(settings.checkMethod)-1);
      strncpy(settings.targets, doc["targets"] | settings.targets, sizeof(settings.targets)-1);
      settings.monitorInterval = doc["monitorInterval"] | settings.monitorInterval;
      settings.ntpInterval = doc["ntpInterval"] | settings.ntpInterval;
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
  String csv = "Start Time,End Time,Duration (seconds),Duration (min:sec),Reason\n";
  for (auto& o : outages) {
    char startStr[30], endStr[30];
    time_t startT = o.startEpoch, endT = o.endEpoch;
    strftime(startStr, sizeof(startStr), "%Y-%m-%d %H:%M:%S", localtime(&startT));
    strftime(endStr, sizeof(endStr), "%Y-%m-%d %H:%M:%S", localtime(&endT));
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
  Serial.println("\n🚀 ESP32 Network Monitor Starting...");
  
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
  Serial.println("⚡ MONITORING ACTIVE\n");
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
