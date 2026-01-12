/*
 * Storage Module - NVS-based persistent storage
 * Replaces EEPROM with wear-leveled NVS storage
 */

#ifndef STORAGE_H
#define STORAGE_H

#include <Preferences.h>

// Configuration
#define MAX_DEVICES 10
#define MAX_LOG_ENTRIES 50
#define DEVICE_NAME_LEN 20

// Device structure (extended with name)
struct StoredDevice {
    uint8_t irk[16];
    char name[DEVICE_NAME_LEN];
    bool active;
};

// Log entry structure
struct LogEntry {
    uint32_t timestamp;     // millis() at event time
    uint8_t deviceIndex;    // Which device (0-9)
    uint8_t action;         // 0=Lock, 1=Unlock
    int8_t rssi;            // Signal strength
};

// Settings structure
struct KeylessSettings {
    int8_t rssiUnlockThreshold;   // Default: -90
    int8_t rssiLockThreshold;     // Default: -80
    uint8_t proximityTimeout;     // Default: 10 (seconds)
    uint8_t weakSignalThreshold;  // Default: 3
};

class Storage {
private:
    Preferences prefs;

public:
    StoredDevice devices[MAX_DEVICES];
    int deviceCount = 0;

    LogEntry logBuffer[MAX_LOG_ENTRIES];
    uint8_t logHead = 0;    // Next write position
    uint8_t logCount = 0;   // Total entries (max 50)

    // Settings with defaults
    KeylessSettings settings = {-90, -80, 10, 3};

    bool begin() {
        return prefs.begin("keyless", false);
    }

    // ========== Device Storage ==========

    bool loadDevices() {
        deviceCount = prefs.getInt("devCount", 0);
        if (deviceCount < 0 || deviceCount > MAX_DEVICES) {
            deviceCount = 0;
            return false;
        }

        for (int i = 0; i < deviceCount; i++) {
            char key[16];

            // Load IRK
            snprintf(key, sizeof(key), "irk%d", i);
            prefs.getBytes(key, devices[i].irk, 16);

            // Load name
            snprintf(key, sizeof(key), "name%d", i);
            prefs.getString(key, devices[i].name, DEVICE_NAME_LEN);

            // Load active state
            snprintf(key, sizeof(key), "act%d", i);
            devices[i].active = prefs.getBool(key, true);
        }

        return deviceCount > 0;
    }

    void saveDevices() {
        prefs.putInt("devCount", deviceCount);

        for (int i = 0; i < deviceCount; i++) {
            char key[16];

            snprintf(key, sizeof(key), "irk%d", i);
            prefs.putBytes(key, devices[i].irk, 16);

            snprintf(key, sizeof(key), "name%d", i);
            prefs.putString(key, devices[i].name);

            snprintf(key, sizeof(key), "act%d", i);
            prefs.putBool(key, devices[i].active);
        }
    }

    bool addDevice(uint8_t* irk, const char* name) {
        if (deviceCount >= MAX_DEVICES) return false;

        // Check for duplicate
        for (int i = 0; i < deviceCount; i++) {
            if (memcmp(devices[i].irk, irk, 16) == 0) {
                return false; // Already exists
            }
        }

        memcpy(devices[deviceCount].irk, irk, 16);
        strncpy(devices[deviceCount].name, name, DEVICE_NAME_LEN - 1);
        devices[deviceCount].name[DEVICE_NAME_LEN - 1] = '\0';
        devices[deviceCount].active = true;
        deviceCount++;

        saveDevices();
        return true;
    }

    bool renameDevice(int index, const char* newName) {
        if (index < 0 || index >= deviceCount) return false;

        strncpy(devices[index].name, newName, DEVICE_NAME_LEN - 1);
        devices[index].name[DEVICE_NAME_LEN - 1] = '\0';

        char key[16];
        snprintf(key, sizeof(key), "name%d", index);
        prefs.putString(key, devices[index].name);

        return true;
    }

    bool deleteDevice(int index) {
        if (index < 0 || index >= deviceCount) return false;

        // Shift remaining devices
        for (int i = index; i < deviceCount - 1; i++) {
            memcpy(&devices[i], &devices[i + 1], sizeof(StoredDevice));
        }
        deviceCount--;

        saveDevices();
        return true;
    }

    // ========== Audit Log Storage ==========

    void loadLog() {
        logHead = prefs.getUChar("logHead", 0);
        logCount = prefs.getUChar("logCount", 0);

        if (logCount > MAX_LOG_ENTRIES) logCount = 0;
        if (logHead >= MAX_LOG_ENTRIES) logHead = 0;

        prefs.getBytes("logBuf", logBuffer, sizeof(logBuffer));
    }

    void addLogEntry(uint8_t deviceIndex, uint8_t action, int8_t rssi, uint32_t timestamp) {
        logBuffer[logHead].timestamp = timestamp;
        logBuffer[logHead].deviceIndex = deviceIndex;
        logBuffer[logHead].action = action;
        logBuffer[logHead].rssi = rssi;

        logHead = (logHead + 1) % MAX_LOG_ENTRIES;
        if (logCount < MAX_LOG_ENTRIES) logCount++;

        // Save to NVS (batched to reduce writes)
        prefs.putUChar("logHead", logHead);
        prefs.putUChar("logCount", logCount);
        prefs.putBytes("logBuf", logBuffer, sizeof(logBuffer));
    }

    // Get log entries in chronological order (oldest first)
    int getLogEntries(LogEntry* output, int maxEntries) {
        if (logCount == 0) return 0;

        int count = min((int)logCount, maxEntries);
        int startIdx;

        if (logCount < MAX_LOG_ENTRIES) {
            startIdx = 0;
        } else {
            startIdx = logHead; // Oldest entry
        }

        for (int i = 0; i < count; i++) {
            int idx = (startIdx + i) % MAX_LOG_ENTRIES;
            output[i] = logBuffer[idx];
        }

        return count;
    }

    // ========== Settings Storage ==========

    void loadSettings() {
        settings.rssiUnlockThreshold = prefs.getChar("rssiUnlock", -90);
        settings.rssiLockThreshold = prefs.getChar("rssiLock", -80);
        settings.proximityTimeout = prefs.getUChar("proxTimeout", 10);
        settings.weakSignalThreshold = prefs.getUChar("weakSigThr", 3);

        Serial.printf("Settings loaded: Unlock=%d, Lock=%d, Timeout=%d, WeakThr=%d\n",
            settings.rssiUnlockThreshold,
            settings.rssiLockThreshold,
            settings.proximityTimeout,
            settings.weakSignalThreshold);
    }

    void saveSettings() {
        prefs.putChar("rssiUnlock", settings.rssiUnlockThreshold);
        prefs.putChar("rssiLock", settings.rssiLockThreshold);
        prefs.putUChar("proxTimeout", settings.proximityTimeout);
        prefs.putUChar("weakSigThr", settings.weakSignalThreshold);

        Serial.println("Settings saved to NVS");
    }

    // ========== Migration from EEPROM ==========

    bool migrateFromEEPROM() {
        // Check if already migrated
        if (prefs.getBool("migrated", false)) {
            return false;
        }

        // Migration will be handled in main.cpp
        // Just mark as migrated after first run
        prefs.putBool("migrated", true);
        return true;
    }
};

#endif // STORAGE_H
