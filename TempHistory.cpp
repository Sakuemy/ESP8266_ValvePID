#include "TempHistory.h"

namespace TempHistory {

static TempPoint buffer_[HISTORY_SIZE];
static size_t writeIndex_ = 0;   // куда будет записана следующая точка
static size_t count_ = 0;        // сколько точек реально заполнено (до HISTORY_SIZE)
static unsigned long lastAddMs_ = 0;

void begin() {
    for (size_t i = 0; i < HISTORY_SIZE; i++) {
        buffer_[i].timestamp = 0;
        buffer_[i].temperature = NAN;
    }
    writeIndex_ = 0;
    count_ = 0;
    lastAddMs_ = 0; // гарантируем, что первая точка добавится сразу после старта
}

void update(float temperature, uint32_t currentUnixTime) {
    if (isnan(temperature)) return;

    unsigned long now = millis();
    if (lastAddMs_ != 0 && (now - lastAddMs_ < HISTORY_INTERVAL_MS)) {
        return;
    }
    lastAddMs_ = now;

    buffer_[writeIndex_].timestamp = currentUnixTime;
    buffer_[writeIndex_].temperature = temperature;

    writeIndex_ = (writeIndex_ + 1) % HISTORY_SIZE;
    if (count_ < HISTORY_SIZE) count_++;
}

size_t count() {
    return count_;
}

size_t serializeToJson(String &out) {
    // Строим JSON вручную потокобезопасно и без лишних больших промежуточных
    // буферов ArduinoJson (история может быть длинной - 288 точек).
    out = "[";
    // Самая старая точка находится по индексу (writeIndex_ - count_ + HISTORY_SIZE) % HISTORY_SIZE
    size_t startIdx = (writeIndex_ + HISTORY_SIZE - count_) % HISTORY_SIZE;

    for (size_t i = 0; i < count_; i++) {
        size_t idx = (startIdx + i) % HISTORY_SIZE;
        if (buffer_[idx].timestamp == 0) continue;

        if (i > 0) out += ",";
        out += "{\"t\":";
        out += String(buffer_[idx].timestamp);
        out += ",\"v\":";
        out += String(buffer_[idx].temperature, 2);
        out += "}";
    }
    out += "]";
    return count_;
}

} // namespace TempHistory
