#include "ValveServo.h"
#include <Servo.h>
#include <math.h>

namespace ValveServo {

static Servo servo;
static ServoSettings settings_;
static double targetPercent_ = 0.0;
static double currentPercent_ = 0.0;
static unsigned long lastUpdateMs_ = 0;
static bool attached_ = false;

// Аварийное закрытие (нет датчика / критический заряд батареи) - см. setSafetyClose().
static bool safety_ = false;

// Предпросмотр калибровки импульсов: пока активен, вместо сохранённых
// closed/open из settings_ используются эти значения.
static bool calibPreview_ = false;
static uint16_t previewClosedUs_ = 0;
static uint16_t previewOpenUs_ = 0;
static unsigned long lastCalibMs_ = 0;

// Импульс, реально выданный на серву (мкс, дробный - для плавного сдвига).
static double pulseOut_ = 0.0;
static uint16_t lastWrittenUs_ = 0;

static Mode mode_ = Mode::AUTO;
static unsigned long modeTimeoutMs_ = 0;   // 0 = без тайм-аута
static unsigned long lastCommandMs_ = 0;   // время последней команды в текущем режиме

// Тест хода
static double testMin_ = 0.0;
static double testMax_ = 100.0;
static uint8_t testStage_ = 0;             // 0 - не идёт, 1..3 - см. getRangeTestStage()
static bool testAtTarget_ = false;         // достигли ли положения текущего этапа
static unsigned long testReachedMs_ = 0;   // когда достигли (для паузы)

static double clampPct(double v, double lo, double hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

// Границы, в которых допустимо целевое положение в текущем режиме:
// в AUTO/MANUAL - min/max из настроек; в PREVIEW/RANGE_TEST - весь ход 0..100
// (иначе нельзя было бы показать ещё не сохранённое значение).
static void allowedRange(double &lo, double &hi) {
    if (mode_ == Mode::PREVIEW || mode_ == Mode::RANGE_TEST) {
        lo = 0.0;
        hi = 100.0;
    } else {
        lo = settings_.minPercent;
        hi = settings_.maxPercent;
    }
}

static bool pulsesValid(uint16_t closedUs, uint16_t openUs) {
    if (closedUs < SERVO_PULSE_MIN_US || closedUs > SERVO_PULSE_MAX_US) return false;
    if (openUs < SERVO_PULSE_MIN_US || openUs > SERVO_PULSE_MAX_US) return false;
    int diff = (int)openUs - (int)closedUs;
    if (diff < 0) diff = -diff;
    return diff >= SERVO_MIN_PULSE_SPAN_US;
}

static uint16_t effClosedUs() { return calibPreview_ ? previewClosedUs_ : settings_.closedPulseUs; }
static uint16_t effOpenUs()   { return calibPreview_ ? previewOpenUs_   : settings_.openPulseUs; }

// Импульс (мкс) для % открытия при текущей (рабочей или предпросматриваемой) калибровке.
static double percentToPulse(double percent) {
    percent = clampPct(percent, 0.0, 100.0);
    double closedUs = effClosedUs();
    double span = (double)effOpenUs() - closedUs;
    return closedUs + span * (percent / 100.0);
}

// Выдать на серву импульс для currentPercent_. Импульс сдвигается не быстрее
// SERVO_MAX_STEP_PERCENT_PER_UPDATE от размаха (open-closed) за такт: при
// обычном движении это ровно тот же шаг, что и у currentPercent_, а при смене
// калибровки (предпросмотр, сохранение, откат) кран не прыгает рывком.
static void writeServo() {
    if (!attached_) return;
    double desired = percentToPulse(currentPercent_);
    double span = (double)effOpenUs() - (double)effClosedUs();
    if (span < 0) span = -span;
    double maxStep = span * SERVO_MAX_STEP_PERCENT_PER_UPDATE / 100.0;
    if (maxStep < 1.0) maxStep = 1.0;
    double d = desired - pulseOut_;
    if (fabs(d) <= maxStep + 0.001) {
        pulseOut_ = desired;
    } else {
        pulseOut_ += (d > 0 ? maxStep : -maxStep);
    }
    uint16_t us = (uint16_t)(pulseOut_ + 0.5);
    if (us != lastWrittenUs_) {
        servo.writeMicroseconds(us);
        lastWrittenUs_ = us;
    }
}

static void setAuto() {
    mode_ = Mode::AUTO;
    modeTimeoutMs_ = 0;
    testStage_ = 0;
}

// Некорректные импульсы (вне диапазона / почти равные) не принимаем -
// остаются прежние (при старте - значения по умолчанию).
static void acceptSettings(const ServoSettings &settings, uint16_t fallbackClosed, uint16_t fallbackOpen) {
    settings_ = settings;
    if (!pulsesValid(settings_.closedPulseUs, settings_.openPulseUs)) {
        Serial.println(F("[ValveServo] Некорректные импульсы калибровки - оставлены прежние"));
        settings_.closedPulseUs = fallbackClosed;
        settings_.openPulseUs = fallbackOpen;
    }
}

void begin(const ServoSettings &settings) {
    ServoSettings defaults;
    acceptSettings(settings, defaults.closedPulseUs, defaults.openPulseUs);
    servo.attach(PIN_SERVO, SERVO_PULSE_MIN_US, SERVO_PULSE_MAX_US); // расширенный диапазон, реальные пределы - калибровка
    attached_ = true;
    lastUpdateMs_ = millis();
    targetPercent_ = settings_.minPercent; // безопасное стартовое положение
    currentPercent_ = settings_.minPercent;
    pulseOut_ = percentToPulse(currentPercent_); // первый импульс - сразу, без сдвига
    lastWrittenUs_ = 0;
    writeServo();
}

void applySettings(const ServoSettings &settings) {
    acceptSettings(settings, settings_.closedPulseUs, settings_.openPulseUs);
    calibPreview_ = false; // сохранённые значения - теперь рабочие
    // В AUTO/MANUAL пересчитываем текущее положение под новые лимиты.
    // Во время предпросмотра/теста - НЕ трогаем: серва намеренно может стоять
    // вне сохранённых min/max, плавно вернётся сама при выходе в AUTO.
    if (mode_ == Mode::AUTO || mode_ == Mode::MANUAL) {
        currentPercent_ = clampPct(currentPercent_, settings_.minPercent, settings_.maxPercent);
    }
    // Импульс сам плавно дойдёт до нового значения на ближайших тактах update().
}

void setSafetyClose(bool active) {
    if (active && !safety_) {
        if (mode_ != Mode::AUTO) {
            Serial.println(F("[ValveServo] Аварийное закрытие - ручной режим/тест/предпросмотр прерваны"));
            setAuto();
        }
        targetPercent_ = settings_.minPercent;
    }
    safety_ = active;
}

bool isSafetyClose() {
    return safety_;
}

void setTargetPercent(double percent) {
    if (mode_ != Mode::AUTO || safety_) return; // ПИД временно не управляет краном / аварийное закрытие
    targetPercent_ = clampPct(percent, 0.0, 100.0);
}

// ---------------------- Ручной режим ----------------------

bool setManualOverride(bool active, unsigned long timeoutMs) {
    if (!active) {
        setAuto();
        return true;
    }
    if (safety_) return false; // аварийное закрытие важнее ручного управления
    mode_ = Mode::MANUAL;
    modeTimeoutMs_ = timeoutMs;
    lastCommandMs_ = millis();
    testStage_ = 0;
    // Стартуем с текущего положения - без рывка при входе в ручной режим.
    targetPercent_ = currentPercent_;
    return true;
}

bool isManualOverrideActive() {
    return mode_ != Mode::AUTO;
}

void setManualPercent(double percent) {
    if (mode_ != Mode::MANUAL || safety_) return;
    targetPercent_ = clampPct(percent, 0.0, 100.0);
    lastCommandMs_ = millis();
}

// ---------------------- Предпросмотр ----------------------

bool previewPercent(double percent) {
    // Не перебиваем идущий тест хода - иначе тест "сломается" посередине.
    if (mode_ == Mode::RANGE_TEST || safety_) return false;
    mode_ = Mode::PREVIEW;
    modeTimeoutMs_ = VALVE_PREVIEW_TIMEOUT_MS;
    lastCommandMs_ = millis();
    targetPercent_ = clampPct(percent, 0.0, 100.0);
    return true;
}

void endPreview() {
    if (mode_ == Mode::PREVIEW) setAuto();
}

// ---------------------- Предпросмотр калибровки ----------------------

bool previewCalibration(uint16_t closedPulseUs, uint16_t openPulseUs) {
    if (!pulsesValid(closedPulseUs, openPulseUs)) return false;
    previewClosedUs_ = closedPulseUs;
    previewOpenUs_ = openPulseUs;
    calibPreview_ = true;
    lastCalibMs_ = millis();
    return true;
}

void endCalibrationPreview() {
    calibPreview_ = false; // импульс плавно вернётся к сохранённым значениям
}

// ---------------------- Тест хода ----------------------

bool startRangeTest(double minP, double maxP) {
    if (safety_) return false;
    minP = clampPct(minP, 0.0, 100.0);
    maxP = clampPct(maxP, 0.0, 100.0);
    if (minP >= maxP) return false;

    testMin_ = minP;
    testMax_ = maxP;
    mode_ = Mode::RANGE_TEST;
    modeTimeoutMs_ = 0; // тест конечен и сам вернёт AUTO
    lastCommandMs_ = millis();
    testStage_ = 1;
    testAtTarget_ = false;
    targetPercent_ = testMin_;
    return true;
}

void stopRangeTest() {
    if (mode_ == Mode::RANGE_TEST) setAuto();
}

bool isRangeTestActive() {
    return mode_ == Mode::RANGE_TEST;
}

uint8_t getRangeTestStage() {
    return mode_ == Mode::RANGE_TEST ? testStage_ : 0;
}

// Конечный автомат теста: вызывается на каждом такте update() ПОСЛЕ
// движения. Этап завершается, когда серва достигла цели и выдержала паузу.
static void runRangeTest(unsigned long now) {
    if (fabs(currentPercent_ - targetPercent_) > 0.05) {
        testAtTarget_ = false;
        return;
    }
    if (!testAtTarget_) {
        testAtTarget_ = true;
        testReachedMs_ = now;
        return;
    }
    if (now - testReachedMs_ < VALVE_RANGE_TEST_DWELL_MS) return;

    testAtTarget_ = false;
    if (testStage_ == 1) {
        testStage_ = 2;
        targetPercent_ = testMax_;
    } else if (testStage_ == 2) {
        testStage_ = 3;
        targetPercent_ = testMin_;
    } else {
        setAuto(); // тест завершён - кран возвращается к ПИД
    }
}

// ---------------------- Основной такт ----------------------

void update() {
    unsigned long now = millis();
    if (now - lastUpdateMs_ < SERVO_UPDATE_INTERVAL_MS) return;
    lastUpdateMs_ = now;

    // Тайм-аут ручного режима/предпросмотра - возврат к ПИД.
    if ((mode_ == Mode::MANUAL || mode_ == Mode::PREVIEW) &&
        modeTimeoutMs_ != 0 && (now - lastCommandMs_ > modeTimeoutMs_)) {
        setAuto();
    }

    // Предпросмотр калибровки без подтверждения - откат к сохранённым импульсам.
    if (calibPreview_ && (now - lastCalibMs_ > VALVE_PREVIEW_TIMEOUT_MS)) {
        calibPreview_ = false;
    }

    // Аварийное закрытие (нет датчика / батарея) перекрывает любую цель.
    if (safety_) targetPercent_ = settings_.minPercent;

    double lo, hi;
    allowedRange(lo, hi);
    double clamped = clampPct(targetPercent_, lo, hi);

    double diff = clamped - currentPercent_;
    if (diff != 0.0) {
        // Плавный ход: не прыгаем сразу на целевое значение, а двигаемся
        // к нему не быстрее SERVO_MAX_STEP_PERCENT_PER_UPDATE за такт -
        // так резкая команда (скачок ПИД, вход/выход из теста) не дёргает
        // кран рывком на весь диапазон за одно обновление.
        if (fabs(diff) <= SERVO_MAX_STEP_PERCENT_PER_UPDATE) {
            currentPercent_ = clamped; // ровно в цель, без накопления погрешности double
        } else {
            currentPercent_ += (diff > 0 ? SERVO_MAX_STEP_PERCENT_PER_UPDATE : -SERVO_MAX_STEP_PERCENT_PER_UPDATE);
        }
    }

    // Импульс выдаём каждый такт: он может двигаться и при неизменном
    // currentPercent_ (плавный сдвиг после смены калибровки).
    writeServo();

    if (mode_ == Mode::RANGE_TEST) runRangeTest(now);
}

double getCurrentPercent() {
    return currentPercent_;
}

double getTargetPercent() {
    return targetPercent_;
}

Mode getMode() {
    return mode_;
}

const char *getModeName() {
    switch (mode_) {
        case Mode::MANUAL:     return "manual";
        case Mode::PREVIEW:    return "preview";
        case Mode::RANGE_TEST: return "test";
        default:               return "auto";
    }
}

uint32_t getModeRemainingSec() {
    if ((mode_ != Mode::MANUAL && mode_ != Mode::PREVIEW) || modeTimeoutMs_ == 0) return 0;
    unsigned long elapsed = millis() - lastCommandMs_;
    if (elapsed >= modeTimeoutMs_) return 0;
    return (uint32_t)((modeTimeoutMs_ - elapsed + 999UL) / 1000UL);
}

} // namespace ValveServo
