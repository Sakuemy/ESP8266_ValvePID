#include "TempSensor.h"
#include <OneWire.h>
#include <DallasTemperature.h>

namespace TempSensor {

static OneWire oneWire(PIN_ONE_WIRE);
static DallasTemperature sensors(&oneWire);
static DeviceAddress sensorAddress;

static bool sensorFound = false;
static bool conversionInProgress = false;
static bool haveValidReading = false;
static float lastTemperature = NAN;
static unsigned long lastActionMs = 0;
static unsigned long lastGoodReadingMs = 0; // millis() момента последнего успешного чтения

bool begin() {
    sensors.begin();
    sensors.setWaitForConversion(false); // не блокируем процессор на время конвертации
    int count = sensors.getDeviceCount();
    if (count <= 0) {
        Serial.println(F("[TempSensor] DS18B20 не найден на шине"));
        sensorFound = false;
        return false;
    }
    if (!sensors.getAddress(sensorAddress, 0)) {
        Serial.println(F("[TempSensor] Не удалось прочитать адрес датчика"));
        sensorFound = false;
        return false;
    }
    sensors.setResolution(sensorAddress, 12);
    sensorFound = true;
    lastActionMs = millis();
    return true;
}

void update() {
    if (!sensorFound) {
        // Если данные уже (были) валидны, но датчик не отвечает достаточно
        // долго - считаем последнее известное значение устаревшим. Именно
        // на haveValidReading завязана защита в main loop (закрытие крана
        // при отсутствии данных), поэтому важно не держать её "включённой"
        // вечно на основании давно неактуального чтения.
        if (haveValidReading && (millis() - lastGoodReadingMs > SENSOR_INVALID_TIMEOUT_MS)) {
            haveValidReading = false;
            Serial.println(F("[TempSensor] Датчик долго не отвечает - последнее значение считается устаревшим"));
        }

        // Периодически пробуем переинициализировать шину — вдруг датчик
        // был подключён позже или произошёл сбой линии.
        if (millis() - lastActionMs > SENSOR_READ_INTERVAL_MS) {
            lastActionMs = millis();
            begin();
        }
        return;
    }

    unsigned long now = millis();

    if (!conversionInProgress) {
        if (now - lastActionMs >= SENSOR_READ_INTERVAL_MS) {
            sensors.requestTemperaturesByAddress(sensorAddress);
            conversionInProgress = true;
            lastActionMs = now;
        }
    } else {
        // 12 бит ~ 750ms максимум
        if (now - lastActionMs >= 800UL) {
            float t = sensors.getTempC(sensorAddress);
            conversionInProgress = false;
            lastActionMs = now;

            if (t == DEVICE_DISCONNECTED_C || t < -55.0f || t > 125.0f) {
                Serial.println(F("[TempSensor] Некорректное чтение, датчик мог отключиться"));
                sensorFound = false; // заставим перепроверить шину при следующем update()
                // haveValidReading здесь намеренно не трогаем - единичный
                // плохой отсчёт может быть наводкой на линии. Он "погаснет"
                // сам, если проблема не исчезнет дольше SENSOR_INVALID_TIMEOUT_MS
                // (см. ветку !sensorFound выше).
            } else {
                lastTemperature = t;
                haveValidReading = true;
                lastGoodReadingMs = now;
            }
        }
    }
}

float getTemperature() {
    return lastTemperature;
}

bool isValid() {
    return haveValidReading;
}

} // namespace TempSensor
