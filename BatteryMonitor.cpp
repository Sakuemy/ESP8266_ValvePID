#include "BatteryMonitor.h"

namespace BatteryMonitor {

static BatterySettings settings_;
static float smoothedVoltage_ = 0.0f;
static bool haveReading_ = false;
static unsigned long lastReadMs_ = 0;

static float rawAdcToVoltage(int adcValue) {
    float v = (adcValue / 1023.0f) * BATTERY_BOARD_ADC_MAX_V * BATTERY_EXTERNAL_DIVIDER_RATIO;
    return v;
}

void begin(const BatterySettings &settings) {
    settings_ = settings;
    // Первое измерение сразу, не дожидаясь интервала, чтобы UI не показывал
    // пустое значение при старте.
    int raw = analogRead(PIN_BATTERY_ADC);
    smoothedVoltage_ = rawAdcToVoltage(raw);
    haveReading_ = true;
    lastReadMs_ = millis();
}

void applySettings(const BatterySettings &settings) {
    settings_ = settings;
}

void update() {
    unsigned long now = millis();
    if (now - lastReadMs_ < BATTERY_READ_INTERVAL_MS) return;
    lastReadMs_ = now;

    // Простое усреднение нескольких быстрых чтений подряд снижает шум АЦП.
    // analogRead() на ESP8266 не блокирует надолго, так что цикл безопасен.
    long sum = 0;
    for (int i = 0; i < BATTERY_SMOOTHING_SAMPLES; i++) {
        sum += analogRead(PIN_BATTERY_ADC);
        delayMicroseconds(200); // короткая пауза между отсчётами, не блокирует loop() заметно
    }
    float avgRaw = (float)sum / BATTERY_SMOOTHING_SAMPLES;
    smoothedVoltage_ = rawAdcToVoltage((int)avgRaw);
    haveReading_ = true;
}

float getVoltage() {
    return haveReading_ ? smoothedVoltage_ : 0.0f;
}

float getPercent() {
    if (!haveReading_) return 0.0f;

    float span = settings_.voltageAt100Percent - settings_.voltageAt0Percent;
    if (fabs(span) < 0.001f) return 0.0f; // защита от деления на ноль при некорректной калибровке

    float percent = (smoothedVoltage_ - settings_.voltageAt0Percent) / span * 100.0f;
    if (percent < 0.0f) percent = 0.0f;
    if (percent > 100.0f) percent = 100.0f;
    return percent;
}

bool isConnected() {
    return haveReading_ && (smoothedVoltage_ >= BATTERY_DISCONNECT_THRESHOLD_V);
}

} // namespace BatteryMonitor
