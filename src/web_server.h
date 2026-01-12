/*
 * Web Server Module - Dashboard and API endpoints
 * Uses standard WebServer (not AsyncWebServer) for lower RAM usage
 */

#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include <WebServer.h>
#include "storage.h"
#include "audit_log.h"
#include "wifi_manager.h"

// External references to global settings variables in main.cpp
extern int RSSI_UNLOCK_THRESHOLD;
extern int RSSI_LOCK_THRESHOLD;
extern unsigned long PROXIMITY_TIMEOUT;
extern int WEAK_SIGNAL_THRESHOLD;

// HTML Dashboard (minified, stored in PROGMEM)
const char DASHBOARD_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html><head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32 Keyless</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:system-ui,-apple-system,sans-serif;background:#1a1a2e;color:#eee;padding:16px;max-width:600px;margin:0 auto}
h1{font-size:1.4em;margin-bottom:16px;color:#4cc9f0}
h2{font-size:1.1em;margin:20px 0 10px;color:#7b2cbf}
.card{background:#16213e;border-radius:8px;padding:12px;margin-bottom:12px}
.device{display:flex;justify-content:space-between;align-items:center;padding:8px 0;border-bottom:1px solid #0f3460}
.device:last-child{border:none}
.device-name{flex:1}
.device-name input{background:#0f3460;border:1px solid #4cc9f0;color:#eee;padding:4px 8px;border-radius:4px;width:140px}
.btn{background:#4cc9f0;color:#1a1a2e;border:none;padding:6px 12px;border-radius:4px;cursor:pointer;font-size:0.9em;margin-left:6px}
.btn:hover{background:#3aa8d8}
.btn-del{background:#e63946}
.btn-del:hover{background:#c92a36}
.btn-save{background:#2d6a4f}
.btn-save:hover{background:#1e4d3a}
.log-entry{display:flex;padding:6px 0;border-bottom:1px solid #0f3460;font-size:0.9em}
.log-entry:last-child{border:none}
.log-time{width:70px;color:#888}
.log-device{flex:1}
.log-action{width:60px;text-align:center;border-radius:4px;padding:2px 6px}
.log-unlock{background:#2d6a4f;color:#fff}
.log-lock{background:#9d0208;color:#fff}
.log-rssi{width:50px;text-align:right;color:#888}
.status{display:flex;gap:16px;font-size:0.85em;color:#888;margin-bottom:16px}
.status span{background:#0f3460;padding:4px 10px;border-radius:4px}
.empty{color:#666;font-style:italic;padding:10px 0}
#msg{position:fixed;bottom:20px;left:50%;transform:translateX(-50%);background:#2d6a4f;padding:10px 20px;border-radius:8px;display:none}
.setting{margin:12px 0}
.setting label{display:block;margin-bottom:4px;font-size:0.9em}
.setting input[type=range]{width:100%;margin:4px 0}
.setting .val{float:right;color:#4cc9f0;font-weight:bold}
.setting small{color:#666;font-size:0.8em}
</style>
</head><body>
<h1>ESP32 Keyless Dashboard</h1>
<div class="status">
<span id="wifi">WiFi: --</span>
<span id="uptime">Uptime: --</span>
</div>
<h2>Devices</h2>
<div class="card" id="devices"><div class="empty">Loading...</div></div>
<h2>Settings</h2>
<div class="card" id="settings">
<div class="setting">
<label>Unlock RSSI Threshold <span class="val" id="v1">-90</span> dBm</label>
<input type="range" id="s1" min="-100" max="-50" value="-90" oninput="$('v1').textContent=this.value">
<small>Signal strength to trigger unlock (lower = longer range)</small>
</div>
<div class="setting">
<label>Lock RSSI Threshold <span class="val" id="v2">-80</span> dBm</label>
<input type="range" id="s2" min="-100" max="-50" value="-80" oninput="$('v2').textContent=this.value">
<small>Signal strength to trigger lock (higher = must be further away)</small>
</div>
<div class="setting">
<label>Lock Timeout <span class="val" id="v3">10</span> sec</label>
<input type="range" id="s3" min="5" max="60" value="10" oninput="$('v3').textContent=this.value">
<small>Time after last detection before locking</small>
</div>
<div class="setting">
<label>Weak Signal Count <span class="val" id="v4">3</span></label>
<input type="range" id="s4" min="1" max="10" value="3" oninput="$('v4').textContent=this.value">
<small>Number of weak signals before triggering lock</small>
</div>
<button class="btn btn-save" onclick="saveSettings()" style="width:100%;margin-top:8px">Save Settings</button>
</div>
<h2>Activity Log</h2>
<div class="card" id="log"><div class="empty">Loading...</div></div>
<div id="msg"></div>
<script>
function $(s){return document.getElementById(s)}
function msg(t){let m=$('msg');m.textContent=t;m.style.display='block';setTimeout(()=>m.style.display='none',2000)}
function load(){
fetch('/api/status').then(r=>r.json()).then(d=>{
$('wifi').textContent='WiFi: '+(d.wifi?d.ip:'Offline');
$('uptime').textContent='Uptime: '+d.uptime;
});
fetch('/api/devices').then(r=>r.json()).then(d=>{
let h='';
d.devices.forEach((dev,i)=>{
if(dev.active){
h+='<div class="device"><div class="device-name"><input id="n'+i+'" value="'+dev.name+'" maxlength="19"></div>';
h+='<button class="btn" onclick="rename('+i+')">Save</button>';
h+='<button class="btn btn-del" onclick="del('+i+')">X</button></div>';
}
});
$('devices').innerHTML=h||'<div class="empty">No devices paired</div>';
});
fetch('/api/log').then(r=>r.json()).then(d=>{
let h='';
d.log.slice().reverse().forEach(e=>{
h+='<div class="log-entry"><span class="log-time">'+e.time+'</span>';
h+='<span class="log-device">'+e.device+'</span>';
h+='<span class="log-action log-'+e.action.toLowerCase()+'">'+e.action+'</span>';
h+='<span class="log-rssi">'+e.rssi+'dB</span></div>';
});
$('log').innerHTML=h||'<div class="empty">No activity yet</div>';
});
fetch('/api/settings').then(r=>r.json()).then(d=>{
$('s1').value=d.rssiUnlock;$('v1').textContent=d.rssiUnlock;
$('s2').value=d.rssiLock;$('v2').textContent=d.rssiLock;
$('s3').value=d.timeout;$('v3').textContent=d.timeout;
$('s4').value=d.weakCount;$('v4').textContent=d.weakCount;
});
}
function rename(i){
let n=$('n'+i).value;
fetch('/api/devices/'+i+'/name',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'name='+encodeURIComponent(n)})
.then(r=>{if(r.ok)msg('Saved!');else msg('Error');load();});
}
function del(i){
if(!confirm('Delete this device?'))return;
fetch('/api/devices/'+i,{method:'DELETE'}).then(r=>{if(r.ok)msg('Deleted');load();});
}
function saveSettings(){
let body='rssiUnlock='+$('s1').value+'&rssiLock='+$('s2').value+'&timeout='+$('s3').value+'&weakCount='+$('s4').value;
fetch('/api/settings',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:body})
.then(r=>{if(r.ok)msg('Settings saved!');else msg('Error');});
}
load();setInterval(load,10000);
</script>
</body></html>
)rawliteral";

class DashboardServer {
private:
    WebServer server;
    Storage* storage;
    AuditLog* auditLog;
    WifiManager* wifiManager;
    unsigned long startTime;

public:
    DashboardServer() : server(80) {}

    void begin(Storage* storagePtr, AuditLog* logPtr, WifiManager* wifiPtr) {
        storage = storagePtr;
        auditLog = logPtr;
        wifiManager = wifiPtr;
        startTime = millis();

        // Dashboard
        server.on("/", HTTP_GET, [this]() {
            server.send_P(200, "text/html", DASHBOARD_HTML);
        });

        // API: Get all devices
        server.on("/api/devices", HTTP_GET, [this]() {
            String json = "{\"devices\":[";
            for (int i = 0; i < storage->deviceCount; i++) {
                if (i > 0) json += ",";
                json += "{\"id\":";
                json += i;
                json += ",\"name\":\"";
                json += storage->devices[i].name;
                json += "\",\"active\":";
                json += storage->devices[i].active ? "true" : "false";
                json += "}";
            }
            json += "]}";
            server.send(200, "application/json", json);
        });

        // API: Rename device
        server.on("/api/devices/0/name", HTTP_POST, [this]() { handleRename(0); });
        server.on("/api/devices/1/name", HTTP_POST, [this]() { handleRename(1); });
        server.on("/api/devices/2/name", HTTP_POST, [this]() { handleRename(2); });
        server.on("/api/devices/3/name", HTTP_POST, [this]() { handleRename(3); });
        server.on("/api/devices/4/name", HTTP_POST, [this]() { handleRename(4); });
        server.on("/api/devices/5/name", HTTP_POST, [this]() { handleRename(5); });
        server.on("/api/devices/6/name", HTTP_POST, [this]() { handleRename(6); });
        server.on("/api/devices/7/name", HTTP_POST, [this]() { handleRename(7); });
        server.on("/api/devices/8/name", HTTP_POST, [this]() { handleRename(8); });
        server.on("/api/devices/9/name", HTTP_POST, [this]() { handleRename(9); });

        // API: Delete device
        server.on("/api/devices/0", HTTP_DELETE, [this]() { handleDelete(0); });
        server.on("/api/devices/1", HTTP_DELETE, [this]() { handleDelete(1); });
        server.on("/api/devices/2", HTTP_DELETE, [this]() { handleDelete(2); });
        server.on("/api/devices/3", HTTP_DELETE, [this]() { handleDelete(3); });
        server.on("/api/devices/4", HTTP_DELETE, [this]() { handleDelete(4); });
        server.on("/api/devices/5", HTTP_DELETE, [this]() { handleDelete(5); });
        server.on("/api/devices/6", HTTP_DELETE, [this]() { handleDelete(6); });
        server.on("/api/devices/7", HTTP_DELETE, [this]() { handleDelete(7); });
        server.on("/api/devices/8", HTTP_DELETE, [this]() { handleDelete(8); });
        server.on("/api/devices/9", HTTP_DELETE, [this]() { handleDelete(9); });

        // API: Get log
        server.on("/api/log", HTTP_GET, [this]() {
            LogEntry entries[MAX_LOG_ENTRIES];
            int count = storage->getLogEntries(entries, MAX_LOG_ENTRIES);

            String json = "{\"log\":[";
            for (int i = 0; i < count; i++) {
                if (i > 0) json += ",";

                char entryJson[128];
                const char* deviceName = "Unknown";
                if (entries[i].deviceIndex < storage->deviceCount) {
                    deviceName = storage->devices[entries[i].deviceIndex].name;
                }
                auditLog->getLogEntryJson(&entries[i], entryJson, sizeof(entryJson), deviceName);
                json += entryJson;
            }
            json += "]}";
            server.send(200, "application/json", json);
        });

        // API: Get settings
        server.on("/api/settings", HTTP_GET, [this]() {
            String json = "{";
            json += "\"rssiUnlock\":";
            json += storage->settings.rssiUnlockThreshold;
            json += ",\"rssiLock\":";
            json += storage->settings.rssiLockThreshold;
            json += ",\"timeout\":";
            json += storage->settings.proximityTimeout;
            json += ",\"weakCount\":";
            json += storage->settings.weakSignalThreshold;
            json += "}";
            server.send(200, "application/json", json);
        });

        // API: Save settings
        server.on("/api/settings", HTTP_POST, [this]() {
            bool changed = false;

            if (server.hasArg("rssiUnlock")) {
                storage->settings.rssiUnlockThreshold = server.arg("rssiUnlock").toInt();
                changed = true;
            }
            if (server.hasArg("rssiLock")) {
                storage->settings.rssiLockThreshold = server.arg("rssiLock").toInt();
                changed = true;
            }
            if (server.hasArg("timeout")) {
                storage->settings.proximityTimeout = server.arg("timeout").toInt();
                changed = true;
            }
            if (server.hasArg("weakCount")) {
                storage->settings.weakSignalThreshold = server.arg("weakCount").toInt();
                changed = true;
            }

            if (changed) {
                storage->saveSettings();

                // Apply settings to global variables immediately
                RSSI_UNLOCK_THRESHOLD = storage->settings.rssiUnlockThreshold;
                RSSI_LOCK_THRESHOLD = storage->settings.rssiLockThreshold;
                PROXIMITY_TIMEOUT = storage->settings.proximityTimeout * 1000UL;
                WEAK_SIGNAL_THRESHOLD = storage->settings.weakSignalThreshold;

                Serial.printf("Settings applied: Unlock=%d, Lock=%d, Timeout=%lums, WeakThr=%d\n",
                    RSSI_UNLOCK_THRESHOLD,
                    RSSI_LOCK_THRESHOLD,
                    PROXIMITY_TIMEOUT,
                    WEAK_SIGNAL_THRESHOLD);
                server.send(200, "application/json", "{\"success\":true}");
            } else {
                server.send(400, "application/json", "{\"error\":\"No settings provided\"}");
            }
        });

        // API: System status
        server.on("/api/status", HTTP_GET, [this]() {
            unsigned long uptime = (millis() - startTime) / 1000;
            unsigned long hours = uptime / 3600;
            unsigned long minutes = (uptime % 3600) / 60;

            String json = "{";
            json += "\"wifi\":";
            json += wifiManager->isConnected() ? "true" : "false";
            json += ",\"ip\":\"";
            json += wifiManager->ipAddress;
            json += "\",\"uptime\":\"";
            json += hours;
            json += "h ";
            json += minutes;
            json += "m\",\"devices\":";
            json += storage->deviceCount;
            json += ",\"logEntries\":";
            json += auditLog->getEntryCount();
            json += ",\"ntpSynced\":";
            json += auditLog->isNtpSynced() ? "true" : "false";
            json += "}";

            server.send(200, "application/json", json);
        });

        server.begin();
        Serial.println("Web server started on port 80");
    }

    void handleClient() {
        server.handleClient();
    }

private:
    void handleRename(int index) {
        if (server.hasArg("name")) {
            String newName = server.arg("name");
            if (storage->renameDevice(index, newName.c_str())) {
                server.send(200, "application/json", "{\"success\":true}");
                Serial.printf("Device %d renamed to: %s\n", index, newName.c_str());
            } else {
                server.send(400, "application/json", "{\"error\":\"Invalid index\"}");
            }
        } else {
            server.send(400, "application/json", "{\"error\":\"Missing name\"}");
        }
    }

    void handleDelete(int index) {
        if (storage->deleteDevice(index)) {
            server.send(200, "application/json", "{\"success\":true}");
            Serial.printf("Device %d deleted\n", index);
        } else {
            server.send(400, "application/json", "{\"error\":\"Invalid index\"}");
        }
    }
};

#endif // WEB_SERVER_H
