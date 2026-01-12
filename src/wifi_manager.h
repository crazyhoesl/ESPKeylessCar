/*
 * WiFi Manager - WiFi client with AP setup mode and NTP sync
 * First boot: Creates AP "ESP32-Keyless-Setup" for WiFi configuration
 * After config: Connects as client to configured network
 */

#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <time.h>
#include "audit_log.h"

// AP Configuration
#define AP_SSID "ESP32-Keyless-Setup"
#define AP_PASS "keyless123"

// NTP configuration
#define NTP_SERVER "pool.ntp.org"
#define GMT_OFFSET_SEC 3600      // UTC+1 (Germany)
#define DAYLIGHT_OFFSET_SEC 3600 // Daylight saving time

// Reconnect settings
#define WIFI_RECONNECT_INTERVAL 30000
#define WIFI_CONNECT_TIMEOUT 15000

// Setup portal HTML
const char SETUP_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html><head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32 Keyless - WiFi Setup</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:system-ui,-apple-system,sans-serif;background:#1a1a2e;color:#eee;padding:20px;max-width:400px;margin:0 auto}
h1{font-size:1.3em;margin-bottom:20px;color:#4cc9f0;text-align:center}
.card{background:#16213e;border-radius:8px;padding:16px;margin-bottom:16px}
label{display:block;margin-bottom:6px;font-size:0.9em;color:#888}
input,select{width:100%;padding:10px;margin-bottom:12px;border:1px solid #4cc9f0;border-radius:4px;background:#0f3460;color:#eee;font-size:1em}
select{cursor:pointer}
.btn{width:100%;background:#4cc9f0;color:#1a1a2e;border:none;padding:12px;border-radius:4px;cursor:pointer;font-size:1em;font-weight:bold}
.btn:hover{background:#3aa8d8}
.btn:disabled{background:#666;cursor:not-allowed}
.info{font-size:0.8em;color:#666;text-align:center;margin-top:16px}
.scanning{color:#4cc9f0;text-align:center;padding:20px}
.network{padding:8px;margin:4px 0;background:#0f3460;border-radius:4px;cursor:pointer;display:flex;justify-content:space-between}
.network:hover{background:#1a3a6e}
.signal{color:#4cc9f0;font-size:0.9em}
#status{margin-top:12px;padding:10px;border-radius:4px;text-align:center;display:none}
.success{background:#2d6a4f;display:block!important}
.error{background:#9d0208;display:block!important}
</style>
</head><body>
<h1>ESP32 Keyless<br>WiFi Setup</h1>
<div class="card">
<div id="networks"><div class="scanning">Scanning networks...</div></div>
</div>
<div class="card">
<form id="form" onsubmit="return save()">
<label>WiFi Network (SSID)</label>
<input type="text" id="ssid" required placeholder="Select from list or type manually">
<label>Password</label>
<input type="password" id="pass" placeholder="WiFi password">
<button type="submit" class="btn" id="btn">Connect</button>
<div id="status"></div>
</form>
</div>
<div class="info">After connecting, the device will restart<br>and connect to your WiFi network.</div>
<script>
function scan(){
fetch('/scan').then(r=>r.json()).then(d=>{
let h='';
d.networks.forEach(n=>{
h+='<div class="network" onclick="sel(\''+n.ssid+'\')"><span>'+n.ssid+'</span><span class="signal">'+n.rssi+' dBm</span></div>';
});
document.getElementById('networks').innerHTML=h||'<div class="scanning">No networks found</div>';
}).catch(()=>{
document.getElementById('networks').innerHTML='<div class="scanning">Scan failed - refresh page</div>';
});
}
function sel(s){document.getElementById('ssid').value=s;}
function save(){
let ssid=document.getElementById('ssid').value;
let pass=document.getElementById('pass').value;
let btn=document.getElementById('btn');
let status=document.getElementById('status');
btn.disabled=true;btn.textContent='Connecting...';
status.className='';status.style.display='none';
fetch('/save',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},
body:'ssid='+encodeURIComponent(ssid)+'&pass='+encodeURIComponent(pass)})
.then(r=>r.json()).then(d=>{
if(d.success){
status.textContent='Connected! Restarting...';
status.className='success';
}else{
status.textContent='Connection failed: '+d.error;
status.className='error';
btn.disabled=false;btn.textContent='Connect';
}
}).catch(()=>{
status.textContent='Error - try again';
status.className='error';
btn.disabled=false;btn.textContent='Connect';
});
return false;
}
scan();
</script>
</body></html>
)rawliteral";

class WifiManager {
private:
    Preferences wifiPrefs;
    AuditLog* auditLog;
    WebServer* setupServer = nullptr;
    DNSServer* dnsServer = nullptr;

    bool configured = false;
    bool connected = false;
    bool apMode = false;
    bool ntpInitialized = false;
    unsigned long lastReconnectAttempt = 0;
    unsigned long connectStartTime = 0;
    bool connecting = false;

    String storedSSID = "";
    String storedPass = "";

    void loadCredentials() {
        wifiPrefs.begin("wificreds", true);  // Read-only
        storedSSID = wifiPrefs.getString("ssid", "");
        storedPass = wifiPrefs.getString("pass", "");
        wifiPrefs.end();

        configured = (storedSSID.length() > 0);
        Serial.printf("WiFi credentials %s\n", configured ? "found" : "not found");
    }

    void saveCredentials(const String& ssid, const String& pass) {
        wifiPrefs.begin("wificreds", false);
        wifiPrefs.putString("ssid", ssid);
        wifiPrefs.putString("pass", pass);
        wifiPrefs.end();

        storedSSID = ssid;
        storedPass = pass;
        configured = true;
        Serial.printf("WiFi credentials saved for: %s\n", ssid.c_str());
    }

    void startAPMode() {
        apMode = true;
        Serial.println("Starting WiFi Setup AP...");

        WiFi.mode(WIFI_AP);
        WiFi.softAP(AP_SSID, AP_PASS);

        IPAddress apIP = WiFi.softAPIP();
        Serial.printf("AP started: %s\n", AP_SSID);
        Serial.printf("Password: %s\n", AP_PASS);
        Serial.printf("Config URL: http://%s/\n", apIP.toString().c_str());

        // DNS server for captive portal
        dnsServer = new DNSServer();
        dnsServer->start(53, "*", apIP);

        // Setup web server
        setupServer = new WebServer(80);

        setupServer->on("/", HTTP_GET, [this]() {
            setupServer->send_P(200, "text/html", SETUP_HTML);
        });

        setupServer->on("/scan", HTTP_GET, [this]() {
            String json = "{\"networks\":[";
            int n = WiFi.scanNetworks();
            for (int i = 0; i < n; i++) {
                if (i > 0) json += ",";
                json += "{\"ssid\":\"";
                json += WiFi.SSID(i);
                json += "\",\"rssi\":";
                json += WiFi.RSSI(i);
                json += "}";
            }
            json += "]}";
            setupServer->send(200, "application/json", json);
        });

        setupServer->on("/save", HTTP_POST, [this]() {
            String ssid = setupServer->arg("ssid");
            String pass = setupServer->arg("pass");

            if (ssid.length() == 0) {
                setupServer->send(200, "application/json", "{\"success\":false,\"error\":\"SSID required\"}");
                return;
            }

            // Try to connect
            Serial.printf("Trying to connect to: %s\n", ssid.c_str());
            WiFi.mode(WIFI_STA);
            WiFi.begin(ssid.c_str(), pass.c_str());

            int attempts = 0;
            while (WiFi.status() != WL_CONNECTED && attempts < 20) {
                delay(500);
                Serial.print(".");
                attempts++;
            }
            Serial.println();

            if (WiFi.status() == WL_CONNECTED) {
                Serial.printf("Connected! IP: %s\n", WiFi.localIP().toString().c_str());
                saveCredentials(ssid, pass);
                setupServer->send(200, "application/json", "{\"success\":true}");
                delay(1000);
                ESP.restart();
            } else {
                Serial.println("Connection failed");
                WiFi.mode(WIFI_AP);
                WiFi.softAP(AP_SSID, AP_PASS);
                setupServer->send(200, "application/json", "{\"success\":false,\"error\":\"Could not connect\"}");
            }
        });

        // Captive portal - redirect all requests
        setupServer->onNotFound([this]() {
            setupServer->sendHeader("Location", "/", true);
            setupServer->send(302, "text/plain", "");
        });

        setupServer->begin();
        Serial.println("Setup portal started");
    }

public:
    String ipAddress = "";

    void begin(AuditLog* logPtr) {
        auditLog = logPtr;
        loadCredentials();

        if (!configured) {
            startAPMode();
        } else {
            WiFi.mode(WIFI_STA);
            WiFi.setAutoReconnect(true);
            Serial.printf("WiFi manager ready for: %s\n", storedSSID.c_str());
        }
    }

    void connect() {
        if (apMode) return;  // Don't connect in AP mode

        if (WiFi.status() == WL_CONNECTED) {
            if (!connected) {
                connected = true;
                ipAddress = WiFi.localIP().toString();
                Serial.printf("WiFi connected! IP: %s\n", ipAddress.c_str());
                syncNTP();
            }
            return;
        }

        if (!connecting && configured) {
            Serial.printf("Connecting to WiFi '%s'...\n", storedSSID.c_str());
            WiFi.begin(storedSSID.c_str(), storedPass.c_str());
            connecting = true;
            connectStartTime = millis();
        }

        if (connecting && (millis() - connectStartTime > WIFI_CONNECT_TIMEOUT)) {
            connecting = false;
            Serial.println("WiFi connection timeout");
        }
    }

    void update() {
        // Handle AP mode
        if (apMode) {
            if (dnsServer) dnsServer->processNextRequest();
            if (setupServer) setupServer->handleClient();
            return;
        }

        // Handle client mode
        if (WiFi.status() == WL_CONNECTED) {
            if (!connected) {
                connected = true;
                connecting = false;
                ipAddress = WiFi.localIP().toString();
                Serial.printf("WiFi connected! IP: %s\n", ipAddress.c_str());
                syncNTP();
            }
        } else {
            if (connected) {
                connected = false;
                ipAddress = "";
                Serial.println("WiFi disconnected");
            }

            if (!connecting && (millis() - lastReconnectAttempt > WIFI_RECONNECT_INTERVAL)) {
                lastReconnectAttempt = millis();
                connect();
            }
        }
    }

    void syncNTP() {
        if (ntpInitialized || !auditLog) return;

        Serial.println("Syncing NTP time...");
        configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);

        struct tm timeinfo;
        int retries = 0;
        while (!getLocalTime(&timeinfo) && retries < 10) {
            delay(500);
            retries++;
        }

        if (retries < 10) {
            time_t now;
            time(&now);
            auditLog->setNtpSync(now);
            ntpInitialized = true;

            char timeStr[32];
            strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", &timeinfo);
            Serial.printf("NTP synced: %s\n", timeStr);
        } else {
            Serial.println("NTP sync failed");
        }
    }

    bool isConnected() {
        return connected;
    }

    bool isAPMode() {
        return apMode;
    }

    int getRSSI() {
        return WiFi.RSSI();
    }

    // Clear stored credentials (for reset)
    void clearCredentials() {
        wifiPrefs.begin("wificreds", false);
        wifiPrefs.clear();
        wifiPrefs.end();
        configured = false;
        Serial.println("WiFi credentials cleared");
    }
};

#endif // WIFI_MANAGER_H
