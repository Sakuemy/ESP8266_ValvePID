#include "ValveServo.h"
#include <Servo.h>

namespace ValveServo {

static Servo servo;
static ServoSettings settings_;
static double targetPercent_ = 0.0;
static double currentPercent_ = 0.0;
static unsigned long lastUpdateMs_ = 0;
static bool attached_ = false;
static bool manualOverride_ = false; // true во время ручного тестового поворота крана

static uint16_t percentToPulse(double percent) {
    // Ограничиваем в первую очередь конфигурационными пределами крана
    if (percent < settings_.minPercent) percent = settings_.minPercent;
    if (percent > settings_.maxPercent) percent = settings_.maxPercent;

    double span = (double)settings_.openPulseUs - (double)settings_.closedPulseUs;
    double pulse = settings_.closedPulseUs + (span * (percent / 100.0));
    return (uint16_t)pulse;
}

void begin(const ServoSettings &settings) {
    settings_ = settings;
    servo.attach(PIN_SERVO, 500, 2500); // расширенный диапазон, реальные лимиты - в percentToPulse
    attached_ = true;
    lastUpdateMs_ = millis();
    setTargetPercent(settings_.minPercent); // безопасное стартовое положение
    currentPercent_ = settings_.minPercent;
    servo.writeMicroseconds(percentToPulse(currentPercent_));
}

void applySettings(const ServoSettings &settings) {
    settings_ = settings;
    // Пересчитать текущее положение под новые лимиты немедленно
    if (currentPercent_ < settings_.minPercent) currentPercent_ = settings_.minPercent;
    if (currentPercent_ > settings_.maxPercent) currentPercent_ = settings_.maxPercent;
    if (attached_) {
        servo.writeMicroseconds(percentToPulse(currentPercent_));
    }
}

void setTargetPercent(double percent) {
    if (manualOverride_) return; // ручной тест активен - ПИД временно не управляет краном
    if (percent < 0.0) percent = 0.0;
    if (percent > 100.0) percent = 100.0;
    targetPercent_ = percent;
}

void setManualOverride(bool active) {
    manualOverride_ = active;
}

bool isManualOverrideActive() {
    return manualOverride_;
}

void setManualPercent(double percent) {
    if (percent < 0.0) percent = 0.0;
    if (percent > 100.0) percent = 100.0;
    targetPercent_ = percent;
}

void update() {
    unsigned long now = millis();
    if (now - lastUpdateMs_ < SERVO_UPDATE_INTERVAL_MS) return;
    lastUpdateMs_ = now;

    double clamped = targetPercent_;
    if (clamped < settings_.minPercent) clamped = settings_.minPercent;
    if (clamped > settings_.maxPercent) clamped = settings_.maxPercent;

    if (clamped != currentPercent_) {
        // Плавный ход: не прыгаем сразу на целевое значение, а двигаемся
        // к нему не быстрее SERVO_MAX_STEP_PERCENT_PER_UPDATE за такт -
        // так резкая команда (скачок ПИД, вход/выход из теста) не дёргает
        // кран рывком на весь диапазон за одно обновление.
        double diff = clamped - currentPercent_;
        if (diff > SERVO_MAX_STEP_PERCENT_PER_UPDATE) diff = SERVO_MAX_STEP_PERCENT_PER_UPDATE;
        if (diff < -SERVO_MAX_STEP_PERCENT_PER_UPDATE) diff = -SERVO_MAX_STEP_PERCENT_PER_UPDATE;
        currentPercent_ += diff;

        if (attached_) {
            servo.writeMicroseconds(percentToPulse(currentPercent_));
        }
    }
}

double getCurrentPercent() {
    return currentPercent_;
}

} // namespace ValveServo
