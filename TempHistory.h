#pragma once
/*
 * TempHistory.h
 * -----------------------------------------------------------------
 * Кольцевой буфер точек температуры за последние 24 часа.
 * Хранится ТОЛЬКО в оперативной памяти согласно ТЗ (не пишется в Flash,
 * чтобы не изнашивать её частыми записями).
 * -----------------------------------------------------------------
 */

#include "Config.h"

namespace TempHistory {

void begin();

// Вызывать часто из loop(); сама решает, пора ли добавить новую точку
// (раз в HISTORY_INTERVAL_MS), используя currentUnixTime как метку времени.
void update(float temperature, uint32_t currentUnixTime);

// Сериализовать историю в компактный JSON-массив [{"t":unixtime,"v":temp}, ...]
// в порядке от старой к новой точке. Возвращает количество точек.
size_t serializeToJson(String &out);

size_t count();

} // namespace TempHistory
