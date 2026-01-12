/*
 * ESP32 Dynamic iPhone Keyless System v7.1
 * Author: crazyhoesl
 * Date: 27. Juli 2025
 * Updated: January 2026 - Added Web Dashboard
 *
 * Features:
 * - Dynamic IRK learning via BLE pairing
 * - Up to 10 devices supported
 * - NVS storage for persistent IRKs + Audit Log
 * - Web Dashboard for device management
 * - WiFi client with NTP time sync
 * - LED status feedback
 * - Full serial debugging
 */

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <BLEAdvertising.h>
#include "mbedtls/aes.h"
#include "esp_task_wdt.h"
#include "esp_gap_ble_api.h"
#include "esp_gattc_api.h"
#include "esp_gatt_defs.h"
#include "esp_bt_main.h"
#include "esp_bt_defs.h"
#include "EEPROM.h"

// New modules for Web Dashboard
#include "storage.h"
#include "audit_log.h"
#include "wifi_manager.h"
#include "web_server.h"

// ========================================
// CONFIGURATION
// ========================================
#define MAX_DEVICES 10
#define EEPROM_SIZE 512
#define PAIRING_TIMEOUT_MS 30000  // 30 seconds pairing window

// Pin definitions
const int LED_PIN = 2;
const int KEY_POWER_PIN = 23;
const int LOCK_BUTTON_PIN = 19;
const int UNLOCK_BUTTON_PIN = 18;

// BLE Service UUIDs for pairing mode - Health/Fitness device
#define DEVICE_INFO_SERVICE_UUID    "180A"  // Device Information Service
#define BATTERY_SERVICE_UUID        "180F"  // Battery Service  
#define HEART_RATE_SERVICE_UUID     "180D"  // Heart Rate Service (makes it look like fitness tracker)
#define CHARACTERISTIC_UUID         "2A37"  // Heart Rate Measurement

// Keyless system parameters
const int SCAN_TIME = 3;
// Default values - actual values loaded from storage.settings
unsigned long PROXIMITY_TIMEOUT = 10000;
int RSSI_UNLOCK_THRESHOLD = -90;  // Öffnen bei schwächerem Signal (größere Reichweite)
int RSSI_LOCK_THRESHOLD = -80;    // Schließen bei stärkerem Signal (kleinere Reichweite)
const unsigned long POWER_OFF_DELAY = 10000;
const unsigned long UNLOCK_DELAY = 500;
const unsigned long LOCK_STABILIZATION_DELAY = 10;

// Hysteresis parameters - WEAK_SIGNAL_THRESHOLD loaded from storage.settings
int WEAK_SIGNAL_THRESHOLD = 3;
const unsigned long WEAK_SIGNAL_RESET_TIME = 5000;

// ========================================
// EEPROM STRUCTURE
// ========================================
#define MAGIC_BYTES_ADDR 0
#define DEVICE_COUNT_ADDR 4
#define DEVICES_START_ADDR 8
#define DEVICE_ENTRY_SIZE 32  // 16 bytes IRK + 16 bytes name
#define MAGIC_VALUE 0xDEADBEEF

struct DeviceEntry {
    uint8_t irk[16];
    char name[16];
};

// ========================================
// GLOBAL VARIABLES
// ========================================

// System state
enum SystemMode {
    MODE_PAIRING,
    MODE_KEYLESS
};
volatile SystemMode currentMode = MODE_PAIRING;

// BLE objects
BLEServer* pServer = NULL;
BLEScan* pBLEScan = NULL;

// Pairing state
bool deviceConnected = false;
unsigned long pairingStartTime = 0;
int extractedDeviceCount = 0;

// Device storage
DeviceEntry knownDevices[MAX_DEVICES];
int numKnownDevices = 0;

// Keyless system state
volatile unsigned long lastSeenTime[MAX_DEVICES] = {0};
volatile bool deviceNearby[MAX_DEVICES] = {false};
volatile bool anyPhoneNearby = false;
volatile bool keyPowered = false;
volatile bool lockTriggered = false;
volatile bool unlockTriggered = false;
volatile bool pendingLock = false;
volatile unsigned long keyPowerTime = 0;
volatile unsigned long lockTriggerTime = 0;

// Hysteresis per device
struct DeviceHysteresis {
    volatile int weakSignalCount;
    volatile unsigned long lastWeakSignalTime;
    volatile bool isWeak;
};
volatile DeviceHysteresis deviceHysteresis[MAX_DEVICES];

// LED control
unsigned long lastLedBlink = 0;
bool ledState = false;

// ========================================
// WEB DASHBOARD MODULES
// ========================================
Storage storage;
AuditLog auditLog;
WifiManager wifiManager;
DashboardServer dashboardServer;
int lastUnlockDevice = -1;  // Track which device triggered last unlock

// ========================================
// LED CONTROL FUNCTIONS
// ========================================

void setLED(bool state) {
    digitalWrite(LED_PIN, state);
    ledState = state;
}

void blinkLED(unsigned long interval) {
    if (millis() - lastLedBlink >= interval) {
        setLED(!ledState);
        lastLedBlink = millis();
    }
}

void blinkPattern(int count, unsigned long onTime, unsigned long offTime) {
    for (int i = 0; i < count; i++) {
        setLED(true);
        delay(onTime);
        setLED(false);
        if (i < count - 1) delay(offTime);
    }
}

// ========================================
// EEPROM FUNCTIONS
// ========================================

void saveDevicesToEEPROM() {
    Serial.println("💾 Saving devices to EEPROM...");
    
    EEPROM.writeULong(MAGIC_BYTES_ADDR, MAGIC_VALUE);
    EEPROM.writeInt(DEVICE_COUNT_ADDR, numKnownDevices);
    
    for (int i = 0; i < numKnownDevices; i++) {
        int addr = DEVICES_START_ADDR + (i * DEVICE_ENTRY_SIZE);
        for (int j = 0; j < 16; j++) {
            EEPROM.writeByte(addr + j, knownDevices[i].irk[j]);
        }
        for (int j = 0; j < 16; j++) {
            EEPROM.writeByte(addr + 16 + j, knownDevices[i].name[j]);
        }
    }
    
    EEPROM.commit();
    Serial.printf("✅ %d devices saved to EEPROM!\n", numKnownDevices);
}

bool loadDevicesFromEEPROM() {
    Serial.println("📖 Loading devices from EEPROM...");
    
    uint32_t magic = EEPROM.readULong(MAGIC_BYTES_ADDR);
    if (magic != MAGIC_VALUE) {
        Serial.println("❌ No valid device data found in EEPROM");
        return false;
    }
    
    numKnownDevices = EEPROM.readInt(DEVICE_COUNT_ADDR);
    if (numKnownDevices < 0 || numKnownDevices > MAX_DEVICES) {
        Serial.println("❌ Invalid device count in EEPROM");
        numKnownDevices = 0;
        return false;
    }
    
    for (int i = 0; i < numKnownDevices; i++) {
        int addr = DEVICES_START_ADDR + (i * DEVICE_ENTRY_SIZE);
        for (int j = 0; j < 16; j++) {
            knownDevices[i].irk[j] = EEPROM.readByte(addr + j);
        }
        for (int j = 0; j < 16; j++) {
            knownDevices[i].name[j] = EEPROM.readByte(addr + 16 + j);
        }
        knownDevices[i].name[15] = '\0'; // Ensure null termination
    }
    
    Serial.printf("✅ %d devices loaded from EEPROM:\n", numKnownDevices);
    for (int i = 0; i < numKnownDevices; i++) {
        Serial.printf("  %d: %s - ", i + 1, knownDevices[i].name);
        for (int j = 0; j < 16; j++) {
            if (knownDevices[i].irk[j] < 16) Serial.print("0");
            Serial.print(knownDevices[i].irk[j], HEX);
        }
        Serial.println();
    }
    
    return true;
}

void addDevice(uint8_t* irk, const char* name) {
    if (numKnownDevices >= MAX_DEVICES) {
        Serial.println("❌ Maximum device limit reached!");
        blinkPattern(2, 1000, 500); // 2x long blink = storage full
        return;
    }

    // Check if device already exists
    for (int i = 0; i < numKnownDevices; i++) {
        if (memcmp(knownDevices[i].irk, irk, 16) == 0) {
            Serial.printf("⚠️ Device %s already known, skipping...\n", name);
            return;
        }
    }

    // Add new device to legacy array
    memcpy(knownDevices[numKnownDevices].irk, irk, 16);
    strncpy(knownDevices[numKnownDevices].name, name, 15);
    knownDevices[numKnownDevices].name[15] = '\0';
    numKnownDevices++;

    // Also save to NVS storage
    storage.addDevice(irk, name);

    Serial.printf("✅ Added device: %s\n", name);
    saveDevicesToEEPROM();  // Keep EEPROM as backup

    blinkPattern(3, 200, 200); // 3x short blink = device added
}

// ========================================
// CRYPTO FUNCTIONS
// ========================================

void aes128_ecb_fast(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]) {
    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);
    mbedtls_aes_setkey_enc(&ctx, key, 128);
    mbedtls_aes_crypt_ecb(&ctx, MBEDTLS_AES_ENCRYPT, in, out);
    mbedtls_aes_free(&ctx);
}

int verifyRPA(const uint8_t* rpaAddress) {
    if ((rpaAddress[0] & 0xC0) != 0x40) {
        return -1; // Not a valid RPA
    }
    
    uint8_t prand[3] = {rpaAddress[0], rpaAddress[1], rpaAddress[2]};
    uint8_t rpaHash[3] = {rpaAddress[3], rpaAddress[4], rpaAddress[5]};
    
    uint8_t input[16] = {0};
    input[13] = prand[0];
    input[14] = prand[1];
    input[15] = prand[2];
    
    for (int deviceIndex = 0; deviceIndex < numKnownDevices; deviceIndex++) {
        uint8_t aesResult[16];
        aes128_ecb_fast(knownDevices[deviceIndex].irk, input, aesResult);
        
        uint8_t reversedResult[16];
        for (int i = 0; i < 16; i++) {
            reversedResult[i] = aesResult[15 - i];
        }
        
        uint8_t computedHash[3] = {reversedResult[0], reversedResult[1], reversedResult[2]};
        
        bool matches = (computedHash[0] == rpaHash[2] && 
                       computedHash[1] == rpaHash[1] && 
                       computedHash[2] == rpaHash[0]);
        
        if (matches) {
            return deviceIndex;
        }
    }
    
    return -1;
}

// ========================================
// KEYLESS SYSTEM FUNCTIONS
// ========================================

void activateKeyPower() {
    if (!keyPowered) {
        digitalWrite(KEY_POWER_PIN, HIGH);
        keyPowered = true;
        keyPowerTime = millis();
        Serial.println("🔌 Key power activated");
    }
}

void deactivateKeyPower() {
    if (keyPowered) {
        digitalWrite(KEY_POWER_PIN, LOW);
        keyPowered = false;
        Serial.println("🔌 Key power deactivated");
    }
}

void triggerLock() {
    digitalWrite(LOCK_BUTTON_PIN, HIGH);
    delay(100);
    digitalWrite(LOCK_BUTTON_PIN, LOW);
    lockTriggered = true;
    lockTriggerTime = millis();
    Serial.println("🔒 Lock triggered");

    // Log lock event
    if (lastUnlockDevice >= 0 && lastUnlockDevice < numKnownDevices) {
        auditLog.logEvent(lastUnlockDevice, ACTION_LOCK, -99);
    }
}

void triggerUnlock() {
    digitalWrite(UNLOCK_BUTTON_PIN, HIGH);
    delay(100);
    digitalWrite(UNLOCK_BUTTON_PIN, LOW);
    unlockTriggered = true;
    Serial.println("🔓 Unlock triggered");

    // Log unlock event (device and RSSI set by scan callback)
}

void handleAllPhonesGone(const char* reason) {
    anyPhoneNearby = false;
    for (int i = 0; i < numKnownDevices; i++) {
        deviceNearby[i] = false;
        deviceHysteresis[i].weakSignalCount = 0;
        deviceHysteresis[i].isWeak = false;
    }
    
    setLED(false);
    Serial.printf("📱 All phones gone (%s)\n", reason);
    
    if (!lockTriggered && !pendingLock) {
        lockTriggerTime = millis() + LOCK_STABILIZATION_DELAY;
        pendingLock = true;
        unlockTriggered = true;
        Serial.println("🔒 Lock scheduled");
    }
}

// ========================================
// BLE PAIRING CLASSES
// ========================================

class MyServerCallbacks: public BLEServerCallbacks {
    void onConnect(BLEServer* pServer) {
        deviceConnected = true;
        Serial.println("=======================================");
        Serial.println("=== DEVICE CONNECTED ===");
        Serial.println("📱 iPhone connected for pairing!");
        Serial.println("=======================================");
    }

    void onDisconnect(BLEServer* pServer) {
        deviceConnected = false;
        Serial.println("=== DEVICE DISCONNECTED ===");
        delay(500);
        if (currentMode == MODE_PAIRING) {
            pServer->startAdvertising();
            Serial.println("🔄 Restarting advertising...");
        }
    }
};

class MySecurity : public BLESecurityCallbacks {
    uint32_t onPassKeyRequest() {
        Serial.println("=== PASSKEY REQUEST ===");
        Serial.println("📱 iPhone requesting passkey...");
        return 123456;
    }

    void onPassKeyNotify(uint32_t pass_key) {
        Serial.printf("=== PASSKEY NOTIFY: %d ===\n", pass_key);
    }

    bool onConfirmPIN(uint32_t pass_key) {
        Serial.printf("=== CONFIRM PIN: %d ===\n", pass_key);
        return (pass_key == 123456); // Only accept our fixed PIN
    }

    bool onSecurityRequest() {
        Serial.println("=== SECURITY REQUEST ===");
        return true;
    }

    void onAuthenticationComplete(esp_ble_auth_cmpl_t cmpl) {
        Serial.println("=======================================");
        Serial.println("=== AUTHENTICATION COMPLETE ===");
        
        if (cmpl.success) {
            Serial.println("🎉 PAIRING SUCCESSFUL!");
            
            esp_bd_addr_t bda;
            memcpy(bda, cmpl.bd_addr, 6);
            
            Serial.print("📱 iPhone Address: ");
            for (int i = 0; i < 6; i++) {
                if (bda[i] < 16) Serial.print("0");
                Serial.print(bda[i], HEX);
                if (i < 5) Serial.print(":");
            }
            Serial.println();
            
            // Extract IRK
            int dev_num = esp_ble_get_bond_device_num();
            if (dev_num > 0) {
                esp_ble_bond_dev_t *bond_device_list = (esp_ble_bond_dev_t *)malloc(sizeof(esp_ble_bond_dev_t) * dev_num);
                esp_ble_get_bond_device_list(&dev_num, bond_device_list);
                
                for (int i = 0; i < dev_num; i++) {
                    bool isCurrentDevice = true;
                    for (int j = 0; j < 6; j++) {
                        if (bond_device_list[i].bd_addr[j] != bda[j]) {
                            isCurrentDevice = false;
                            break;
                        }
                    }
                    
                    if (isCurrentDevice) {
                        // Create device name
                        char deviceName[16];
                        snprintf(deviceName, sizeof(deviceName), "Device_%02d", numKnownDevices + 1);
                        
                        // Fix IRK byte order - ESP32 BLE stack returns IRK in reverse order
                        uint8_t correctedIRK[16];
                        for (int k = 0; k < 16; k++) {
                            correctedIRK[k] = bond_device_list[i].bond_key.pid_key.irk[15 - k];
                        }
                        
                        Serial.println("🔍 Original IRK (as received):");
                        for (int k = 0; k < 16; k++) {
                            if (bond_device_list[i].bond_key.pid_key.irk[k] < 16) Serial.print("0");
                            Serial.print(bond_device_list[i].bond_key.pid_key.irk[k], HEX);
                        }
                        Serial.println();
                        
                        Serial.println("🔄 Corrected IRK (byte-reversed):");
                        for (int k = 0; k < 16; k++) {
                            if (correctedIRK[k] < 16) Serial.print("0");
                            Serial.print(correctedIRK[k], HEX);
                        }
                        Serial.println();
                        
                        // Add device with corrected IRK
                        addDevice(correctedIRK, deviceName);
                        
                        Serial.println("🔑 IRK successfully extracted and saved!");
                        Serial.println("🔄 Restarting ESP32 for clean BLE initialization...");
                        delay(2000);
                        ESP.restart(); // Clean restart for proper BLE mode switching
                        break;
                    }
                }
                free(bond_device_list);
            }
        } else {
            Serial.printf("❌ PAIRING FAILED! Reason: %d\n", cmpl.fail_reason);
            blinkPattern(5, 100, 100); // Very fast blink = error
        }
        Serial.println("=======================================");
    }
};

// ========================================
// BLE KEYLESS SCAN CALLBACK
// ========================================

class MyAdvertisedDeviceCallbacks: public BLEAdvertisedDeviceCallbacks {
    void onResult(BLEAdvertisedDevice advertisedDevice) {
        if (currentMode != MODE_KEYLESS) return;
        
        esp_bd_addr_t* addr = advertisedDevice.getAddress().getNative();
        
        if (((*addr)[0] & 0xC0) == 0x40) { // RPA check
            int matchedDevice = verifyRPA((uint8_t*)(*addr));
            if (matchedDevice >= 0) {
                int rssi = advertisedDevice.getRSSI();
                const char* deviceName = knownDevices[matchedDevice].name;
                
                // Check for unlock (more sensitive, longer range)
                if (rssi > RSSI_UNLOCK_THRESHOLD) {
                    lastSeenTime[matchedDevice] = millis();
                    deviceHysteresis[matchedDevice].weakSignalCount = 0;
                    deviceHysteresis[matchedDevice].isWeak = false;
                    
                    bool wasAnyPhoneNearby = anyPhoneNearby;
                    deviceNearby[matchedDevice] = true;
                    
                    // Update overall status
                    anyPhoneNearby = false;
                    for (int i = 0; i < numKnownDevices; i++) {
                        if (deviceNearby[i]) {
                            anyPhoneNearby = true;
                            break;
                        }
                    }
                    
                    // Removed frequent serial output to prevent TX buffer blocking WiFi

                    // Immediate unlock on first strong signal
                    if (!wasAnyPhoneNearby && anyPhoneNearby) {
                        setLED(true);
                        activateKeyPower();
                        lockTriggered = false;
                        unlockTriggered = false;
                        pendingLock = false;
                        lastUnlockDevice = matchedDevice;  // Track device for logging
                        auditLog.logEvent(matchedDevice, ACTION_UNLOCK, rssi);  // Log unlock
                        Serial.println("🔓 Welcome! Activating unlock sequence...");
                    }
                } else if (rssi <= RSSI_LOCK_THRESHOLD) {
                    // Check for lock (less sensitive, shorter range)
                    // Weak signal hysteresis
                    if (millis() - deviceHysteresis[matchedDevice].lastWeakSignalTime > WEAK_SIGNAL_RESET_TIME) {
                        deviceHysteresis[matchedDevice].weakSignalCount = 0;
                    }
                    deviceHysteresis[matchedDevice].lastWeakSignalTime = millis();
                    
                    if (++deviceHysteresis[matchedDevice].weakSignalCount >= WEAK_SIGNAL_THRESHOLD) {
                        deviceHysteresis[matchedDevice].isWeak = true;
                        deviceNearby[matchedDevice] = false;
                        
                        // Update overall status
                        anyPhoneNearby = false;
                        for (int i = 0; i < numKnownDevices; i++) {
                            if (deviceNearby[i]) {
                                anyPhoneNearby = true;
                                break;
                            }
                        }
                        
                        // Removed: Serial output blocked WiFi when no monitor connected

                        if (!anyPhoneNearby) {
                            handleAllPhonesGone("weak signal hysteresis");
                        }
                    }
                } else {
                    // RSSI between -90 and -80: Maintain current state, update last seen time
                    if (deviceNearby[matchedDevice]) {
                        lastSeenTime[matchedDevice] = millis();
                        // Removed: Serial output blocked WiFi when no monitor connected
                    }
                }
            }
        }
    }
};

// ========================================
// MODE SWITCHING FUNCTIONS
// ========================================

void startPairingMode() {
    currentMode = MODE_PAIRING;
    pairingStartTime = millis();
    
    Serial.println("🔵 Starting BLE pairing mode...");
    
    // Initialize BLE for pairing
    BLEDevice::init("ESPKV7 Tracker");
    BLEDevice::setEncryptionLevel(ESP_BLE_SEC_ENCRYPT);
    BLEDevice::setSecurityCallbacks(new MySecurity());
    
    // Create server
    pServer = BLEDevice::createServer();
    pServer->setCallbacks(new MyServerCallbacks());
    
    // Create Device Information Service
    BLEService *pDeviceInfoService = pServer->createService(DEVICE_INFO_SERVICE_UUID);
    BLECharacteristic *pManufacturerChar = pDeviceInfoService->createCharacteristic(
        "2A29", BLECharacteristic::PROPERTY_READ
    );
    pManufacturerChar->setValue("ESPKV7 Tech");
    
    BLECharacteristic *pModelChar = pDeviceInfoService->createCharacteristic(
        "2A24", BLECharacteristic::PROPERTY_READ
    );
    pModelChar->setValue("KeyTracker Pro");
    
    BLECharacteristic *pFirmwareChar = pDeviceInfoService->createCharacteristic(
        "2A26", BLECharacteristic::PROPERTY_READ
    );
    pFirmwareChar->setValue("v7.0");
    pDeviceInfoService->start();
    
    // Create Battery Service
    BLEService *pBatteryService = pServer->createService(BATTERY_SERVICE_UUID);
    BLECharacteristic *pBatteryChar = pBatteryService->createCharacteristic(
        "2A19", BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY
    );
    pBatteryChar->addDescriptor(new BLE2902());
    uint8_t batteryLevel = 85; // Fake 85% battery
    pBatteryChar->setValue(&batteryLevel, 1);
    pBatteryService->start();
    
    // Create Heart Rate Service (main pairing service)
    BLEService *pHeartRateService = pServer->createService(HEART_RATE_SERVICE_UUID);
    BLECharacteristic *pHeartRateChar = pHeartRateService->createCharacteristic(
        CHARACTERISTIC_UUID,
        BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY
    );
    pHeartRateChar->addDescriptor(new BLE2902());
    uint8_t heartRateData[2] = {0x00, 75}; // Fake 75 BPM
    pHeartRateChar->setValue(heartRateData, 2);
    pHeartRateService->start();
    
    // Configure security
    esp_ble_auth_req_t auth_req = ESP_LE_AUTH_REQ_SC_BOND;
    esp_ble_io_cap_t iocap = ESP_IO_CAP_KBDISP;
    uint8_t key_size = 16;
    uint8_t init_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    uint8_t rsp_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    
    esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth_req, sizeof(uint8_t));
    esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &iocap, sizeof(uint8_t));
    esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &key_size, sizeof(uint8_t));
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &init_key, sizeof(uint8_t));
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &rsp_key, sizeof(uint8_t));
    
    // Start advertising as fitness tracker
    BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
    
    // Configure for iPhone fitness device compatibility
    pAdvertising->addServiceUUID(HEART_RATE_SERVICE_UUID);  // Heart rate service first
    pAdvertising->addServiceUUID(BATTERY_SERVICE_UUID);     // Battery service
    pAdvertising->addServiceUUID(DEVICE_INFO_SERVICE_UUID); // Device info service
    pAdvertising->setScanResponse(true);
    pAdvertising->setMinPreferred(0x20);   // 20ms min interval
    pAdvertising->setMaxPreferred(0x40);   // 40ms max interval
    
    // Set advertisement data with fitness tracker appearance
    BLEAdvertisementData advertisementData;
    advertisementData.setName("ESPKV7 Tracker");
    advertisementData.setAppearance(0x0340); // Generic: Heart Rate Sensor
    advertisementData.setCompleteServices(BLEUUID(HEART_RATE_SERVICE_UUID));
    advertisementData.setFlags(0x06); // General discoverable mode + BR/EDR not supported
    
    pAdvertising->setAdvertisementData(advertisementData);
    
    // Set scan response data with additional services
    BLEAdvertisementData scanResponseData;
    scanResponseData.setName("ESPKV7 Tracker");
    scanResponseData.setPartialServices(BLEUUID(BATTERY_SERVICE_UUID));
    pAdvertising->setScanResponseData(scanResponseData);
    
    BLEDevice::startAdvertising();
    
    Serial.println("🚀 ESPKV7 Tracker advertising started!");
    Serial.println("📱 Go to iPhone Settings > Bluetooth and look for 'ESPKV7 Tracker' as fitness device");
    if (numKnownDevices > 0) {
        Serial.printf("⏱️ 30s window to add more devices (or automatic keyless mode after timeout)\n");
    } else {
        Serial.printf("⏱️ Pairing window: %d seconds\n", PAIRING_TIMEOUT_MS / 1000);
    }
}

void startKeylessMode() {
    currentMode = MODE_KEYLESS;
    
    Serial.println("🔐 Starting keyless mode...");
    
    // Stop advertising if running
    if (pServer) {
        pServer->getAdvertising()->stop();
    }
    
    // Complete BLE shutdown and restart for clean scanner mode
    Serial.println("🔄 Reinitializing BLE stack for scanning...");
    BLEDevice::deinit(true);
    delay(2000);  // Extended delay for complete BLE stack cleanup
    
    // Restart BLE in scanner mode
    BLEDevice::init("");
    delay(500);   // Allow BLE stack to initialize
    
    // Setup scanner with robust error handling
    pBLEScan = BLEDevice::getScan();
    if (pBLEScan) {
        Serial.println("📡 Setting up BLE scanner...");
        pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
        pBLEScan->setActiveScan(false);  // Passive scanning is more reliable
        
        // Wait for BLE to be ready, then set parameters
        delay(1000);
        
        // Use very conservative scan parameters to avoid errors
        pBLEScan->setInterval(1600);  // 1000ms intervals
        pBLEScan->setWindow(800);     // 500ms windows
        
        Serial.println("📡 BLE scanner ready - parameters set successfully");
    } else {
        Serial.println("❌ Failed to create BLE scanner");
    }
    
    // Initialize hysteresis
    for (int i = 0; i < numKnownDevices; i++) {
        deviceHysteresis[i].weakSignalCount = 0;
        deviceHysteresis[i].lastWeakSignalTime = 0;
        deviceHysteresis[i].isWeak = false;
        deviceNearby[i] = false;
        lastSeenTime[i] = 0;
    }
    
    Serial.println("✅ Keyless system ready - monitoring for known devices");
    setLED(true); // Solid LED = keyless mode active
    delay(2000);
    setLED(false);
}

// ========================================
// MAIN SETUP
// ========================================

void setup() {
    Serial.begin(115200);
    delay(1000);

    Serial.println("=======================================");
    Serial.println("🔵 ESP32 Dynamic Keyless System v7.1");
    Serial.println("    + Web Dashboard");
    Serial.println("=======================================");

    // Initialize EEPROM (for migration)
    EEPROM.begin(EEPROM_SIZE);

    // Initialize NVS Storage
    if (!storage.begin()) {
        Serial.println("Failed to initialize NVS storage!");
    }

    // Load settings from NVS and apply to global variables
    storage.loadSettings();
    RSSI_UNLOCK_THRESHOLD = storage.settings.rssiUnlockThreshold;
    RSSI_LOCK_THRESHOLD = storage.settings.rssiLockThreshold;
    PROXIMITY_TIMEOUT = storage.settings.proximityTimeout * 1000UL;  // Convert to ms
    WEAK_SIGNAL_THRESHOLD = storage.settings.weakSignalThreshold;

    // Initialize pins
    pinMode(LED_PIN, OUTPUT);
    pinMode(KEY_POWER_PIN, OUTPUT);
    pinMode(LOCK_BUTTON_PIN, OUTPUT);
    pinMode(UNLOCK_BUTTON_PIN, OUTPUT);

    setLED(false);
    digitalWrite(KEY_POWER_PIN, LOW);
    digitalWrite(LOCK_BUTTON_PIN, LOW);
    digitalWrite(UNLOCK_BUTTON_PIN, LOW);

    // Initialize watchdog
    esp_task_wdt_init(30, true);
    esp_task_wdt_add(NULL);
    Serial.println("🐕 Watchdog enabled (30s timeout)");

    // Check reset reason to avoid endless restart loop
    esp_reset_reason_t resetReason = esp_reset_reason();
    Serial.printf("🔍 Reset reason: %d\n", resetReason);

    // Initialize Audit Log
    auditLog.begin(&storage);

    // Initialize WiFi Manager
    wifiManager.begin(&auditLog);
    wifiManager.connect();

    // Load existing devices from NVS first
    bool hasNvsDevices = storage.loadDevices();

    // Migrate from EEPROM if NVS is empty but EEPROM has data
    bool hasDevices = false;
    if (!hasNvsDevices) {
        hasDevices = loadDevicesFromEEPROM();
        if (hasDevices && numKnownDevices > 0) {
            Serial.println("Migrating devices from EEPROM to NVS...");
            for (int i = 0; i < numKnownDevices; i++) {
                storage.addDevice(knownDevices[i].irk, knownDevices[i].name);
            }
            Serial.printf("Migrated %d devices to NVS\n", numKnownDevices);
        }
    } else {
        // Sync NVS devices to legacy array for compatibility
        hasDevices = true;
        numKnownDevices = storage.deviceCount;
        for (int i = 0; i < numKnownDevices; i++) {
            memcpy(knownDevices[i].irk, storage.devices[i].irk, 16);
            strncpy(knownDevices[i].name, storage.devices[i].name, 15);
            knownDevices[i].name[15] = '\0';
        }
        Serial.printf("Loaded %d devices from NVS\n", numKnownDevices);
    }
    
    if (hasDevices && numKnownDevices > 0) {
        Serial.printf("✅ Found %d known devices\n", numKnownDevices);
        for (int i = 0; i < numKnownDevices; i++) {
            Serial.printf("  %d: %s\n", i + 1, knownDevices[i].name);
        }
        
        // If this is a software reset (from our auto-restart), go directly to keyless mode
        if (resetReason == ESP_RST_SW) {
            Serial.println("🔐 Software reset detected - starting keyless mode directly...");
            startKeylessMode();
        } else {
            Serial.println("🔄 Power-on boot - starting 30s pairing window to add more devices...");
            startPairingMode();
        }
    } else {
        Serial.println("❌ No known devices found");
        Serial.println("🔄 Starting pairing mode - connect your iPhone now!");
        startPairingMode();
    }

    // Start Web Dashboard Server
    dashboardServer.begin(&storage, &auditLog, &wifiManager);
    Serial.println("🌐 Web Dashboard ready");
}

// ========================================
// MAIN LOOP
// ========================================

void loop() {
    esp_task_wdt_reset();

    // Update WiFi and Web Server
    wifiManager.update();
    dashboardServer.handleClient();

    if (currentMode == MODE_PAIRING) {
        // Check pairing timeout - but NOT in AP mode (WiFi setup)
        if (millis() - pairingStartTime >= PAIRING_TIMEOUT_MS) {
            Serial.printf("⏱️ Timeout reached. AP Mode: %s\n", wifiManager.isAPMode() ? "YES" : "NO");
            if (wifiManager.isAPMode()) {
                // Don't restart during WiFi setup - just reset timer silently
                pairingStartTime = millis();
            } else if (numKnownDevices > 0) {
                Serial.println("⏰ Pairing timeout - restarting ESP32 for clean keyless mode");
                delay(2000);
                ESP.restart();
            } else {
                Serial.println("⏰ Pairing timeout - no devices paired, restarting pairing...");
                pairingStartTime = millis();
            }
        }
        
        // LED blink pattern for pairing mode
        if (deviceConnected) {
            blinkLED(200); // Fast blink when connected
        } else {
            blinkLED(1000); // Slow blink when waiting
        }
        
    } else { // MODE_KEYLESS
        // BLE scan for known devices with error handling
        esp_task_wdt_reset();
        // Removed frequent serial output to prevent TX buffer blocking WiFi

        BLEScanResults foundDevices;
        try {
            foundDevices = pBLEScan->start(SCAN_TIME, false);
            esp_task_wdt_reset();
        } catch (...) {
            Serial.println("⚠️ BLE scan failed, reinitializing scanner...");
            esp_task_wdt_reset();
            
            // Reinitialize scanner on error
            pBLEScan = BLEDevice::getScan();
            if (pBLEScan) {
                pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
                pBLEScan->setActiveScan(false);
                pBLEScan->setInterval(1600);
                pBLEScan->setWindow(800);
                Serial.println("📡 Scanner reinitialized");
            }
            foundDevices = BLEScanResults(); // Empty result
        }
        
        // Reset weak signal counters if timeout
        for (int i = 0; i < numKnownDevices; i++) {
            if (deviceHysteresis[i].weakSignalCount > 0 && 
                (millis() - deviceHysteresis[i].lastWeakSignalTime > WEAK_SIGNAL_RESET_TIME)) {
                deviceHysteresis[i].weakSignalCount = 0;
                deviceHysteresis[i].isWeak = false;
            }
        }
        
        // Check device timeouts
        for (int i = 0; i < numKnownDevices; i++) {
            if (deviceNearby[i] && (millis() - lastSeenTime[i] > PROXIMITY_TIMEOUT)) {
                deviceNearby[i] = false;
                Serial.printf("📱 %s timeout\n", knownDevices[i].name);
                
                // Update overall status
                anyPhoneNearby = false;
                for (int j = 0; j < numKnownDevices; j++) {
                    if (deviceNearby[j]) {
                        anyPhoneNearby = true;
                        break;
                    }
                }
                
                if (!anyPhoneNearby) {
                    handleAllPhonesGone("device timeout");
                }
            }
        }
        
        // Execute pending lock
        if (pendingLock && millis() >= lockTriggerTime) {
            triggerLock();
            pendingLock = false;
        }
        
        // Trigger unlock after delay
        if (anyPhoneNearby && keyPowered && !unlockTriggered && 
            (millis() - keyPowerTime >= UNLOCK_DELAY)) {
            triggerUnlock();
        }
        
        // Turn off key power after lock
        if (lockTriggered && (millis() - lockTriggerTime >= POWER_OFF_DELAY)) {
            deactivateKeyPower();
            lockTriggered = false;
        }
        
        // LED status: ON when phones nearby, OFF when not
        setLED(anyPhoneNearby);
        
        pBLEScan->clearResults();
        delay(500);
    }
}
