#pragma once
/*
 * Config.h
 * -----------------------------------------------------------------
 * Общие константы, назначение пинов и структуры данных, разделяемые
 * между всеми модулями проекта.
 *
 * ВСЕ пины ниже — предположение по умолчанию для платы NodeMCU/Wemos D1 mini.
 * При необходимости поменяйте под свою разводку.
 * Пины GPIO0, GPIO2, GPIO15 являются "strapping" пинами ESP8266 — старайтесь
 * не вешать на них подтяжки, которые могут помешать загрузке модуля.
 * -----------------------------------------------------------------
 */

#include <Arduino.h>

// ---------------------- Пины ----------------------
#define PIN_ONE_WIRE      D5   // GPIO14 - шина DS18B20
#define PIN_SERVO         D6   // GPIO12 - управляющий сигнал сервопривода
#define PIN_OLED_SDA      D2   // GPIO4  - I2C SDA дисплея
#define PIN_OLED_SCL      D1   // GPIO5  - I2C SCL дисплея
#define PIN_ENC_CLK       D7   // GPIO13 - энкодер CLK (A)
#define PIN_ENC_DT        D8   // GPIO15 - энкодер DT  (B)  (см. примечание в README про strap-пин)
#define PIN_ENC_BTN       D0   // GPIO16 - кнопка энкодера (без поддержки interrupt, читаем поллингом)
#define PIN_BATTERY_ADC   A0   // единственный аналоговый вход ESP8266

// ---------------------- Сеть по умолчанию ----------------------
#define DEFAULT_AP_SSID   "ValvePID-Setup"
#define DEFAULT_AP_PASS   "12345678"
#define WIFI_CONNECT_TIMEOUT_MS   15000UL
#define WIFI_RETRY_INTERVAL_MS    30000UL   // как часто пытаться переподключиться в фоне

// ---------------------- NTP ----------------------
#define NTP_SERVER_1      "pool.ntp.org"
#define NTP_SERVER_2      "time.nist.gov"
#define NTP_GMT_OFFSET_SEC 0        // задаётся дополнительно смещением в TimeManager, если нужно
#define NTP_RESYNC_INTERVAL_MS  (6UL * 60UL * 60UL * 1000UL)  // раз в 6 часов
#define NTP_RETRY_INTERVAL_MS   (60UL * 1000UL)                // при неудаче - раз в минуту

// ---------------------- История температуры ----------------------
// 24 часа с шагом 5 минут = 288 точек. Хранится только в RAM (не переживает перезагрузку).
#define HISTORY_INTERVAL_MS   (5UL * 60UL * 1000UL)
#define HISTORY_SIZE          288

// ---------------------- Батарея ----------------------
// A0 у большинства плат NodeMCU/Wemos D1 mini уже имеет встроенный делитель,
// позволяющий подавать на вход 0..3.3В (сырое АЦП 0..1023 -> 0..3.3В).
// Если используется "голый" модуль ESP-xx без встроенного делителя, ADC
// ограничен 0..1.0В - поставьте BATTERY_BOARD_ADC_MAX_V = 1.0.
// BATTERY_EXTERNAL_DIVIDER_RATIO - дополнительный множитель, если помимо
// встроенного делителя платы вы поставили свой (внешний) делитель, чтобы
// измерять более высоковольтную батарею (например, 2S/3S Li-ion).
#define BATTERY_BOARD_ADC_MAX_V         3.3f
#define BATTERY_EXTERNAL_DIVIDER_RATIO  1.0f
#define BATTERY_READ_INTERVAL_MS        2000UL
#define BATTERY_SMOOTHING_SAMPLES       8
// Если измеренное напряжение ниже этого порога - считаем, что батарея
// физически не подключена (вход "плавает" или подтянут к земле).
#define BATTERY_DISCONNECT_THRESHOLD_V  1.0f

// ---------------------- ПИД / опрос датчика ----------------------
#define SENSOR_READ_INTERVAL_MS   2000UL   // DS18B20 конвертация ~750ms на 12 бит, берём с запасом
#define PID_COMPUTE_INTERVAL_MS   2000UL
#define SERVO_UPDATE_INTERVAL_MS  200UL
#define SETTINGS_AUTOSAVE_DEBOUNCE_MS 3000UL

// ---------------------- Файл настроек ----------------------
#define CONFIG_FILE_PATH   "/config.json"
#define CONFIG_JSON_CAPACITY  1024

// -----------------------------------------------------------------
//                         Структуры данных
// -----------------------------------------------------------------

struct PIDSettings {
    double kp = 20.0;
    double ki = 0.5;
    double kd = 1.0;
    double setpoint = 60.0;     // целевая температура, °C
};

struct ServoSettings {
    uint8_t minPercent = 0;     // минимально допустимый % открытия крана
    uint8_t maxPercent = 100;   // максимально допустимый % открытия крана
    uint16_t closedPulseUs = 1000; // импульс (мкс), соответствующий полностью закрытому крану
    uint16_t openPulseUs   = 2000; // импульс (мкс), соответствующий полностью открытому крану
};

struct NetworkSettings {
    char ssid[33]     = "";
    char password[65] = "";
    bool useDhcp      = true;
    uint32_t staticIp      = 0; // хранится как packed IPAddress (uint32_t)
    uint32_t staticGateway = 0;
    uint32_t staticSubnet  = 0;
    uint32_t staticDns     = 0;
};

struct BatterySettings {
    float voltageAt0Percent   = 3.0f;  // напряжение батареи (В), соответствующее 0% заряда
    float voltageAt100Percent = 4.2f;  // напряжение батареи (В), соответствующее 100% заряда
};

struct AppSettings {
    PIDSettings pid;
    ServoSettings servo;
    NetworkSettings network;
    BatterySettings battery;
};

// Точка истории температуры
struct TempPoint {
    uint32_t timestamp; // unix time (секунды), 0 = пусто/не заполнено
    float temperature;
};
