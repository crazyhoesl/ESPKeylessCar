/*
 * Audit Log Module - Event logging with time synchronization
 * Handles NTP sync and time conversion for log entries
 */

#ifndef AUDIT_LOG_H
#define AUDIT_LOG_H

#include <time.h>
#include "storage.h"

// Action types
#define ACTION_LOCK   0
#define ACTION_UNLOCK 1

class AuditLog {
private:
    Storage* storage;

    // NTP synchronization state
    time_t ntpSyncTime = 0;         // Unix time when NTP synced
    uint32_t ntpSyncMillis = 0;     // millis() when NTP synced
    bool ntpSynced = false;

public:
    void begin(Storage* storagePtr) {
        storage = storagePtr;
        storage->loadLog();
    }

    // Called after NTP sync succeeds
    void setNtpSync(time_t unixTime) {
        ntpSyncTime = unixTime;
        ntpSyncMillis = millis();
        ntpSynced = true;
        Serial.printf("NTP synced: %ld at millis %lu\n", (long)unixTime, ntpSyncMillis);
    }

    bool isNtpSynced() {
        return ntpSynced;
    }

    // Convert millis timestamp to real time (returns 0 if not synced)
    time_t millisToRealTime(uint32_t logMillis) {
        if (!ntpSynced) return 0;

        int32_t diff = (int32_t)(logMillis - ntpSyncMillis);
        return ntpSyncTime + (diff / 1000);
    }

    // Get current time (real or relative)
    uint32_t getCurrentTimestamp() {
        return millis();
    }

    // Log an event
    void logEvent(uint8_t deviceIndex, uint8_t action, int8_t rssi) {
        storage->addLogEntry(deviceIndex, action, rssi, millis());

        Serial.printf("LOG: Device %d, Action %s, RSSI %d\n",
            deviceIndex,
            action == ACTION_UNLOCK ? "UNLOCK" : "LOCK",
            rssi);
    }

    // Format time for display
    void formatTime(uint32_t logMillis, char* buffer, size_t bufSize) {
        if (ntpSynced) {
            time_t realTime = millisToRealTime(logMillis);
            struct tm* timeinfo = localtime(&realTime);
            strftime(buffer, bufSize, "%H:%M:%S", timeinfo);
        } else {
            // Relative time since boot
            uint32_t seconds = logMillis / 1000;
            uint32_t minutes = seconds / 60;
            uint32_t hours = minutes / 60;

            if (hours > 0) {
                snprintf(buffer, bufSize, "+%luh%02lum", hours, minutes % 60);
            } else if (minutes > 0) {
                snprintf(buffer, bufSize, "+%lum%02lus", minutes, seconds % 60);
            } else {
                snprintf(buffer, bufSize, "+%lus", seconds);
            }
        }
    }

    // Get formatted log entry for JSON
    void getLogEntryJson(LogEntry* entry, char* buffer, size_t bufSize, const char* deviceName) {
        char timeStr[16];
        formatTime(entry->timestamp, timeStr, sizeof(timeStr));

        snprintf(buffer, bufSize,
            "{\"time\":\"%s\",\"device\":\"%s\",\"action\":\"%s\",\"rssi\":%d}",
            timeStr,
            deviceName,
            entry->action == ACTION_UNLOCK ? "Unlock" : "Lock",
            entry->rssi);
    }

    int getEntryCount() {
        return storage->logCount;
    }
};

#endif // AUDIT_LOG_H
