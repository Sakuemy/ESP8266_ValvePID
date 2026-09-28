#include "TempHistory.h"

namespace TempHistory {

static TempPoint buffer_[HISTORY_SIZE];
static size_t writeIndex_ = 0;   // куда будет записана следующая точка
static size_t count_ = 0;        // сколько точек реально заполнено (до HISTORY_SIZE)
static unsigned long lastAddMs_ = 0;
// Накопители для усреднения открытия крана между двумя точками истории.
static double valveSum_ = 0.0;
static uint32_t valveSamples_ = 0;

void begin() {
    for (size_t i = 0; i < HISTORY_SIZE; i++) {
        buffer_[i].timestamp = 0;
        buffer_[i].temperature = NAN;
        buffer_[i].valve = 0.0f;
    }
    valveSum_ = 0.0;
    valveSamples_ = 0;
    writeIndex_ = 0;
    count_ = 0;
    lastAddMs_ = 0; // гарантируем, что первая точка добавится сразу после старта
}

void update(float temperature, float valvePercent, uint32_t currentUnixTime) {
    if (isnan(temperature)) return;

    // Копим среднее открытие крана на каждом вызове (loop() вызывает нас с
    // равномерной частотой, так что среднее по вызовам ~ среднее по времени).
    valveSum_ += valvePercent;
    valveSamples_++;

    unsigned long now = millis();
    if (lastAddMs_ != 0 && (now - lastAddMs_ < HISTORY_INTERVAL_MS)) {
        return;
    }
    lastAddMs_ = now;

    buffer_[writeIndex_].timestamp = currentUnixTime;
    buffer_[writeIndex_].temperature = temperature;
    buffer_[writeIndex_].valve = valveSamples_ ? (float)(valveSum_ / valveSamples_) : valvePercent;
    valveSum_ = 0.0;
    valveSamples_ = 0;

    writeIndex_ = (writeIndex_ + 1) % HISTORY_SIZE;
    if (count_ < HISTORY_SIZE) count_++;
}

size_t count() {
    return count_;
}

size_t serializeToJson(String &out) {
    // Строим JSON вручную и без больших промежуточных буферов ArduinoJson
    // (история может быть длинной - 288 точек). Заранее резервируем память
    // под строку одним куском, чтобы не фрагментировать кучу при росте.
    out = "";
    out.reserve(count_ * 44 + 8);
    out += "[";
    // Самая старая точка находится по индексу (writeIndex_ - count_ + HISTORY_SIZE) % HISTORY_SIZE
    size_t startIdx = (writeIndex_ + HISTORY_SIZE - count_) % HISTORY_SIZE;

    bool first = true;
    for (size_t i = 0; i < count_; i++) {
        size_t idx = (startIdx + i) % HISTORY_SIZE;
        if (buffer_[idx].timestamp == 0) continue;

        if (!first) out += ",";
        first = false;
        out += "{\"t\":";
        out += String(buffer_[idx].timestamp);
        out += ",\"v\":";
        out += String(buffer_[idx].temperature, 2);
        out += ",\"s\":";
        out += String(buffer_[idx].valve, 0);
        out += "}";
    }
    out += "]";
    return count_;
}

} // namespace TempHistory
