#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <WiFiClientSecure.h> // 🔧 FIX 1: Added for Telegram HTTPS
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <time.h>

// ==================== CONFIGURATION & SETTINGS ====================
const char* ssid = "🪔";
const char* password = "pollob12";
const int LED_PIN = 8;

// Dynamic Settings Structure
struct SystemSettings {
  char timezone[64] = "CET-1CEST,M3.5.0,M10.5.0/3";
  char checkMethod[16] = "http";
  char targets[256] = "google.com,microsoft.com,cloudflare.com";
  unsigned long monitorInterval = 1000;
  unsigned long ntpInterval = 300000;
  char telegramBotToken[128] = "";
  char telegramChatId[32] = "";
} settings;

// Globals
WebServer server(80);
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 0, 60000); // Offset 0, handled by TZ env

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

const int HISTORY_SIZE = 1440;
bool availabilityHistory[HISTORY_SIZE];
int historyIndex = 0;
unsigned long lastHistoryUpdate = 0;

// ==================== TELEGRAM ====================
void sendTelegramMessage(String message) {
  if (strlen(settings.telegramBotToken) == 0) return;
  WiFiClientSecure client;
  client.setInsecure(); // 🔧 FIX 2: Skip cert verification for Telegram
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
  Serial.printf("🌍 Timezone: %s\n", settings.timezone);
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
  serializeJson(doc, file); file.close();
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
  } else {
    for (int i = 0; i < HISTORY_SIZE; i++) availabilityHistory[i] = true;
    historyIndex = 0; lastHistoryUpdate = 0;
  }
  return true;
}

// ==================== NETWORK TESTS ====================
bool checkTarget(const char* target, const char* method) {
  WiFiClient client; client.setTimeout(800);
  if (strcmp(method, "http") == 0) return client.connect(target, 80);
  if (strcmp(method, "ping") == 0) return client.connect(target, 53) || client.connect(target, 443);
  return false; // fallback
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
  IPAddress r; return (WiFi.hostByName("8.8.8.8", r) == 1);
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

void updateAvailabilityHistory(bool ok) {
  unsigned long now = timeClient.getEpochTime();
  if (now < 1000000000) return;
  if (lastHistoryUpdate == 0) { lastHistoryUpdate = now; return; }
  unsigned long mins = (now - lastHistoryUpdate) / 60;
  if (mins >= 1) {
    for (unsigned long i = 0; i < mins && i < 60; i++) {
      availabilityHistory[historyIndex] = ok;
      historyIndex = (historyIndex + 1) % HISTORY_SIZE;
    }
    lastHistoryUpdate = now;
  }
}

void updateStatistics(bool ok) {
  unsigned long now = timeClient.getEpochTime();
  if (now < 1000000000) return;

  if (ok) {
    if (!lastOverallOk && currentOutage.startEpoch != 0) {
      currentOutage.endEpoch = now;
      currentOutage.durationSec = now - currentOutage.startEpoch;
      if (!gatewayOk) currentOutage.reason = "Gateway Failed";
      else if (!dnsOk) currentOutage.reason = "DNS Failed";
      else currentOutage.reason = "Internet Failed";
      
      outages.push_back(currentOutage);
      if (currentOutage.durationSec > longestOutageSeconds) longestOutageSeconds = currentOutage.durationSec;
      totalDowntimeSeconds += currentOutage.durationSec;
      failedChecks += currentOutage.durationSec;
      
      char msg[150];
      snprintf(msg, sizeof(msg), "🔴 OUTAGE ENDED\nDuration: %lu min\nReason: %s",
               currentOutage.durationSec/60, currentOutage.reason.c_str());
      sendTelegramMessage(msg);
      
      currentOutage = {0,0,0,""};
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
      strftime(ts, sizeof(ts), "%H:%M:%S", localtime(&t));
      snprintf(msg, sizeof(msg), "⚠️ OUTAGE STARTED\nTime: %s", ts);
      sendTelegramMessage(msg);
      lastTransitionTime = now;
    }
    if (lastTransitionTime > 0) totalDowntimeSeconds += (now - lastTransitionTime);
    else totalDowntimeSeconds++;
    failedChecks++;
    lastTransitionTime = now;
  }
  updateAvailabilityHistory(ok);
  lastOverallOk = ok;
}

void handleWiFiReconnect() {
  if (WiFi.status() != WL_CONNECTED && millis() - lastWiFiReconnectAttempt > 30000) {
    lastWiFiReconnectAttempt = millis();
    WiFi.disconnect(); WiFi.begin(ssid, password);
  }
}

// ==================== WEB HANDLERS ====================
void handleRoot() {
  // 🔧 FIX 3: Added Settings link to header
  String html = R"rawliteral(
<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'>
<title>Network Monitor</title><script src='https://cdn.jsdelivr.net/npm/chart.js@4.4.0/dist/chart.umd.min.js'></script>
<style>
*{margin:0;padding:0;box-sizing:border-box}body{background:#0a0e1a;color:#e5e7eb;font-family:system-ui,sans-serif;padding:20px}
.dashboard{max-width:1400px;margin:0 auto}.header{background:#111827;border:1px solid #2d3748;border-radius:16px;padding:20px;margin-bottom:25px;display:flex;justify-content:space-between;align-items:center}
h1{font-size:1.5rem;display:flex;align-items:center;gap:10px}.status-led{width:12px;height:12px;border-radius:50%;background:#ef4444;animation:blink 1s infinite}@keyframes blink{50%{opacity:0.5}}.status-led.online{background:#10b981}
.stats{display:grid;grid-template-columns:repeat(auto-fit,minmax(200px,1fr));gap:20px;margin-bottom:25px}
.card{background:#111827;border:1px solid #2d3748;border-radius:16px;padding:20px;text-align:center}
.label{color:#9ca3af;font-size:0.8rem;margin-bottom:8px}.val{font-size:1.8rem;font-weight:bold;font-family:monospace}
.status-grid{display:grid;grid-template-columns:repeat(4,1fr);gap:20px;margin-bottom:25px}
.s-card{background:#111827;border:2px solid #2d3748;border-radius:16px;padding:20px;text-align:center}
.s-card.ok{border-color:#10b981;background:rgba(16,185,129,0.1)}.s-card.fail{border-color:#ef4444;background:rgba(239,68,68,0.1)}
.btn{background:#3b82f6;color:white;border:none;padding:8px 16px;border-radius:8px;cursor:pointer;text-decoration:none;font-size:0.85rem}
.btn:hover{background:#2563eb}table{width:100%;border-collapse:collapse;margin-top:15px}th,td{padding:12px;border-bottom:1px solid #2d3748;text-align:left}
canvas{max-height:300px}
</style></head><body>
<div class='dashboard'>
<div class='header'>
<div><h1><span class='status-led' id='led'></span> ESP32 Monitor</h1></div>
<div><a href='/settings' class='btn'>⚙️ Settings</a></div>
</div>
<div class='status-grid'>
<div class='s-card' id='overall'><div class='label'>OVERALL</div><div class='val' id='ov'>--</div></div>
<div class='s-card' id='gw'><div class='label'>GATEWAY</div><div class='val' id='gwv'>--</div></div>
<div class='s-card' id='dns'><div class='label'>DNS</div><div class='val' id='dnsv'>--</div></div>
<div class='s-card' id='net'><div class='label'>INTERNET</div><div class='val' id='netv'>--</div></div>
</div>
<div class='stats'>
<div class='card'><div class='label'>UPTIME %</div><div class='val' id='up'>--</div></div>
<div class='card'><div class='label'>CHECKS</div><div class='val' id='chk'>--</div></div>
<div class='card'><div class='label'>LATENCY</div><div class='val' id='lat'>--</div></div>
</div>
<div class='card' style='padding:15px;margin-bottom:25px'><canvas id='chart'></canvas></div>
<div class='card'>
<div style='display:flex;justify-content:space-between;margin-bottom:15px'>
<h3>Outage History</h3>
<div><button class='btn' onclick='exp()'>📥 CSV</button> <button class='btn' style='background:#ef4444' onclick='clr()'>🗑️ Clear</button></div>
</div>
<div style='max-height:300px;overflow:auto'><table id='tbl'><thead><tr><th>Start</th><th>End</th><th>Duration</th><th>Reason</th></tr></thead><tbody></tbody></table></div>
</div>
</div>
<script>
let c;
function r(){
  fetch('/api/status').then(r=>r.json()).then(d=>{
    let ok=d.overall;
    document.getElementById('led').className='status-led '+(ok?'online':'');
    document.getElementById('ov').textContent=ok?'ONLINE':'OFFLINE';
    document.getElementById('gwv').textContent=d.gateway?'OK':'FAIL';
    document.getElementById('dnsv').textContent=d.dns?'OK':'FAIL';
    document.getElementById('netv').textContent=d.internet?'OK':'FAIL';
    ['overall','gw','dns','net'].forEach(id=>{
      let el=document.getElementById(id);
      let s=el.id==='overall'?(ok?'ok':'fail'):(d[el.id==='gw'?'gateway':el.id==='net'?'internet':'dns']?'ok':'fail');
      el.className='s-card '+s;
    });
    document.getElementById('up').textContent=(d.totalUptime/(d.totalUptime+d.totalDowntime)*100).toFixed(2)+'%';
    document.getElementById('chk').textContent=d.totalChecks;
    document.getElementById('lat').textContent=(d.checkDuration/1000).toFixed(0)+'ms';
    let tb=document.querySelector('#tbl tbody');
    tb.innerHTML='';
    if(d.outages)d.outages.reverse().forEach((o,i)=>{
      tb.innerHTML+=`<tr><td>${new Date(o.start*1000).toLocaleString()}</td><td>${new Date(o.end*1000).toLocaleString()}</td><td>${Math.floor(o.duration/60)}m</td><td>${o.reason}</td></tr>`;
    }); else tb.innerHTML='<tr><td colspan=4>None</td></tr>';
    if(c)c.destroy();
    c=new Chart(document.getElementById('chart'),{type:'line',data:{labels:d.historyLabels,datasets:[{label:'Status',data:d.historyData,borderColor:'#10b981',backgroundColor:'rgba(16,185,129,0.2)',fill:true,tension:0.2}]},options:{responsive:true,scales:{y:{min:-0.1,max:1.1,ticks:{callback:v=>v===1?'Up':'Down'}}}}});
  });
}
function exp(){window.location.href='/api/export';}
function clr(){if(confirm('Clear all data?'))fetch('/api/clear',{method:'POST'}).then(r);}
r(); setInterval(r,2000);
</script></body></html>
)rawliteral";
  server.send(200, "text/html", html);
}

// 🔧 FIX 3: handleAPIStatus RESTORED
void handleAPIStatus() {
  StaticJsonDocument<32768> doc;
  doc["overall"] = lastOverallOk && gatewayOk && dnsOk && internetOk;
  doc["gateway"] = gatewayOk; doc["dns"] = dnsOk; doc["internet"] = internetOk;
  doc["totalUptime"] = totalUptimeSeconds; doc["totalDowntime"] = totalDowntimeSeconds;
  doc["totalChecks"] = totalChecks; doc["checkDuration"] = lastCheckDuration;
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
    } else server.send(400, "text/plain", "JSON Error");
  }
}

void handleExportCSV() {
  String csv = "Start,End,Duration(s),Reason\n";
  for (auto& o : outages) {
    char s[30], e[30];
    time_t st = o.startEpoch, et = o.endEpoch;
    strftime(s, sizeof(s), "%Y-%m-%d %H:%M:%S", localtime(&st));
    strftime(e, sizeof(e), "%Y-%m-%d %H:%M:%S", localtime(&et));
    csv += String(s) + "," + String(e) + "," + o.durationSec + "," + o.reason + "\n";
  }
  server.send(200, "text/csv", csv);
}

void handleClearData() {
  outages.clear(); totalDowntimeSeconds = 0; longestOutageSeconds = 0; failedChecks = 0;
  saveDataToLittleFS(); server.send(200, "text/plain", "Cleared");
}

// ==================== SETUP ====================
void setup() {
  Serial.begin(115200); delay(1000);
  if (!LittleFS.begin(false)) if (!LittleFS.begin(true)) Serial.println("FS Error");
  
  loadSettings(); loadDataFromLittleFS();

  WiFi.mode(WIFI_STA); WiFi.begin(ssid, password);
  Serial.print("Connecting"); int a=0;
  while (WiFi.status() != WL_CONNECTED && a < 40) { delay(500); Serial.print("."); a++; }
  if (WiFi.status() == WL_CONNECTED) { Serial.print("\n✓ IP: "); Serial.println(WiFi.localIP()); }
  
  timeClient.begin(); timeClient.update();
  startTime = timeClient.getEpochTime(); if (startTime < 1000000000) startTime = 1700000000;
  lastTransitionTime = startTime;
  if (lastHistoryUpdate == 0) { for(int i=0;i<HISTORY_SIZE;i++) availabilityHistory[i]=true; lastHistoryUpdate=startTime; }

  server.on("/", handleRoot);
  server.on("/settings", handleSettingsPage);
  server.on("/api/status", handleAPIStatus);
  server.on("/api/settings", handleAPISettings);
  server.on("/api/export", handleExportCSV);
  server.on("/api/clear", HTTP_POST, handleClearData);
  server.begin();
  
  lastMonitorTime = millis(); lastSaveTime = millis();
  Serial.println("🚀 Ready");
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
    timeClient.update(); lastNTPUpdate = now;
  }
}
