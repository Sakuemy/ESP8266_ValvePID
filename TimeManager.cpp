#include "TimeManager.h"
#include <time.h>

namespace TimeManager {

static bool syncedOnce_ = false;
static uint32_t lastSyncEpoch_ = 0;      // unix-время на момент последней успешной синхронизации
static unsigned long lastSyncMillis_ = 0; // millis() на тот же момент
static unsigned long lastSyncAttemptMs_ = 0;

void begin() {
    // Настраиваем SNTP. GMT-смещение = 0 (UTC), летнее время не учитываем -
    // при необходимости сместите NTP_GMT_OFFSET_SEC под свой часовой пояс.
    configTime(NTP_GMT_OFFSET_SEC, 0, NTP_SERVER_1, NTP_SERVER_2);
    lastSyncMillis_ = millis();
}

void update(bool wifiConnected) {
    if (!wifiConnected) return;

    unsigned long sinceLastAttempt = millis() - lastSyncAttemptMs_;
    unsigned long requiredInterval = syncedOnce_ ? NTP_RESYNC_INTERVAL_MS : NTP_RETRY_INTERVAL_MS;

    if (lastSyncAttemptMs_ != 0 && sinceLastAttempt < requiredInterval) {
        return;
    }
    lastSyncAttemptMs_ = millis();

    time_t nowSec = time(nullptr);
    // Пока SNTP не синхронизировался, time() возвращает малое значение
    // (близкое к 0, точка отсчёта эпохи). Порог 1700000000 ~ ноябрь 2023.
    if (nowSec > 1700000000UL) {
        lastSyncEpoch_ = (uint32_t)nowSec;
        lastSyncMillis_ = millis();
        syncedOnce_ = true;
    }
}

uint32_t now() {
    if (!syncedOnce_) {
        // Ещё ни разу не синхронизировались - отдаём время с начала работы
        // устройства (эпоха будет некорректной, но монотонно растущей,
        // чего достаточно для относительной привязки точек графика).
        return millis() / 1000UL;
    }
    unsigned long elapsedMs = millis() - lastSyncMillis_;
    return lastSyncEpoch_ + (elapsedMs / 1000UL);
}

bool hasSyncedOnce() {
    return syncedOnce_;
}

uint32_t secondsSinceLastSync() {
    if (!syncedOnce_) return 0;
    return (millis() - lastSyncMillis_) / 1000UL;
}

} // namespace TimeManager
