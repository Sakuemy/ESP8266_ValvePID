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
            } else {
                lastTemperature = t;
                haveValidReading = true;
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
