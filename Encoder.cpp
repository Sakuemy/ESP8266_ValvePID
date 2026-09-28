#include "Encoder.h"

// В новых версиях ESP8266 core обработчики прерываний обязаны быть помечены
// IRAM_ATTR (в старых - ICACHE_RAM_ATTR). Даём запасной вариант для старых.
#ifndef IRAM_ATTR
#define IRAM_ATTR ICACHE_RAM_ATTR
#endif

namespace Encoder {

// Текущее состояние линий: бит1 = CLK(A), бит0 = DT(B).
static volatile uint8_t prevState_ = 0;
// Накопленные квадратурные шаги (по 4 на детент у типового энкодера).
static volatile int32_t steps_ = 0;

// Таблица переходов [предыдущее состояние << 2 | новое состояние] -> шаг.
// Допустимый цикл "вперёд":  11 -> 01 -> 00 -> 10 -> 11  (т.е. CLK падает
// при DT=1 - то же направление, что считал прежний поллинг: +1).
// Все переходы, меняющие сразу оба бита, и "нулевые" переходы - это дребезг
// или пропуск шага, дают 0 и игнорируются. Лежит в RAM (не PROGMEM) -
// читается из обработчика прерывания.
static int8_t TRANSITIONS[16] = {
    /* 00->00 */  0, /* 00->01 */ -1, /* 00->10 */ +1, /* 00->11 */  0,
    /* 01->00 */ +1, /* 01->01 */  0, /* 01->10 */  0, /* 01->11 */ -1,
    /* 10->00 */ -1, /* 10->01 */  0, /* 10->10 */  0, /* 10->11 */ +1,
    /* 11->00 */  0, /* 11->01 */ +1, /* 11->10 */ -1, /* 11->11 */  0
};

// Вызывается из обработчика прерывания - обязана лежать в IRAM.
static uint8_t IRAM_ATTR readState() {
    return (uint8_t)((digitalRead(PIN_ENC_CLK) << 1) | digitalRead(PIN_ENC_DT));
}

static void IRAM_ATTR onEncoderChange() {
    uint8_t s = readState();
    int8_t step = TRANSITIONS[(prevState_ << 2) | s];
    prevState_ = s;
#if ENCODER_REVERSE_DIRECTION
    steps_ -= step;
#else
    steps_ += step;
#endif
}

void begin() {
    pinMode(PIN_ENC_CLK, INPUT_PULLUP);
    pinMode(PIN_ENC_DT, INPUT_PULLUP);
    prevState_ = readState();
    steps_ = 0;
    attachInterrupt(digitalPinToInterrupt(PIN_ENC_CLK), onEncoderChange, CHANGE);
    attachInterrupt(digitalPinToInterrupt(PIN_ENC_DT), onEncoderChange, CHANGE);
}

long takeDelta() {
    // Читаем и вычитаем целое число детентов атомарно относительно ISR.
    // Неполный детент (остаток) остаётся в счётчике до следующего вызова -
    // так медленное вращение не теряет шаги на границе вызовов.
    noInterrupts();
    long detents = steps_ / ENCODER_STEPS_PER_DETENT;
    steps_ -= detents * ENCODER_STEPS_PER_DETENT;
    interrupts();
    return detents;
}

} // namespace Encoder
