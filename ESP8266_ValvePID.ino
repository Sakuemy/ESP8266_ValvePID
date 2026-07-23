/*
 * ESP8266_ValvePID.ino
 * =====================================================================
 * ПИД-регулятор температуры для шарового крана на сервоприводе, ESP8266.
 *
 * Функционал:
 *  - Чтение температуры DS18B20 (неблокирующее).
 *  - ПИД-регулирование % открытия крана.
 *  - Веб-интерфейс: мониторинг (график 24ч), настройки ПИД/сервопривода/Wi-Fi.
 *  - Локальное меню на OLED + энкодере.
 *  - NTP-синхронизация времени с оффлайн-фолбэком.
 *  - Все пользовательские настройки хранятся в LittleFS (JSON),
 *    история температуры - только в RAM.
 *
 * Требуемые библиотеки (Arduino Library Manager):
 *  - OneWire
 *  - DallasTemperature
 *  - ArduinoJson (v6.x)
 *  - U8g2
 *  (Servo, ESP8266WiFi, ESP8266WebServer, LittleFS идут в комплекте с
 *   ESP8266 core для Arduino)
 *
 * Плата: любая ESP8266 (NodeMCU, Wemos D1 mini и т.п.), см. пины в Config.h
 * =====================================================================
 */

#include "Config.h"
#include "Storage.h"
#include "TempSensor.h"
#include "PIDController.h"
#include "ValveServo.h"
#include "TempHistory.h"
#include "TimeManager.h"
#include "NetworkManager.h"
#include "WebServerManager.h"
#include "DisplayMenu.h"
#include "BatteryMonitor.h"

static AppSettings settings;
static PIDController pid;

static unsigned long lastPidComputeMs = 0;
static unsigned long lastSettingsSaveMs = 0;
static bool settingsSavePending = false;

// ---------------------------------------------------------------------
// Общие колбэки для веб-сервера и дисплейного меню: они работают с одним
// и тем же объектом настроек в оперативной памяти и просят сохранить/
// применить его при изменениях, не зная деталей друг о друге.
// ---------------------------------------------------------------------

static AppSettings *getSettingsPtr() {
    return &settings;
}

static void onSettingsChanged() {
    // Применяем изменения немедленно, без перезагрузки устройства.
    pid.configure(settings.pid.kp, settings.pid.ki, settings.pid.kd, settings.pid.setpoint);
    ValveServo::applySettings(settings.servo);
    BatteryMonitor::applySettings(settings.battery);

    // Сохранение в Flash делаем с небольшим дебаунсом на случай, если
    // несколько полей меняются подряд (например, при вводе через веб-форму) -
    // это снижает число операций записи в LittleFS.
    settingsSavePending = true;
    lastSettingsSaveMs = millis();
}

void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println(F("\n[Boot] ESP8266 Valve PID Controller"));

    Storage::begin();
    Storage::load(settings);

    TempSensor::begin();
    ValveServo::begin(settings.servo);
    BatteryMonitor::begin(settings.battery);
    TempHistory::begin();
    TimeManager::begin();

    pid.configure(settings.pid.kp, settings.pid.ki, settings.pid.kd, settings.pid.setpoint);

    NetworkManager::begin(settings.network);

    WebServerManager::Callbacks webCb;
    webCb.getSettings = getSettingsPtr;
    webCb.onSettingsChanged = onSettingsChanged;
    WebServerManager::begin(webCb);

    DisplayMenu::Callbacks dispCb;
    dispCb.getSettings = getSettingsPtr;
    dispCb.onSettingsChanged = onSettingsChanged;
    DisplayMenu::begin(dispCb);

    lastPidComputeMs = millis();
    Serial.println(F("[Boot] Инициализация завершена"));
}

void loop() {
    // Каждый модуль сам решает, когда ему пора выполнить работу -
    // loop() не содержит блокирующих delay() и не переполняется.
    NetworkManager::update();
    TimeManager::update(NetworkManager::isConnected());
    TempSensor::update();
    BatteryMonitor::update();

    unsigned long now = millis();

    if (now - lastPidComputeMs >= PID_COMPUTE_INTERVAL_MS) {
        double dtSeconds = (now - lastPidComputeMs) / 1000.0;
        lastPidComputeMs = now;

        if (TempSensor::isValid()) {
            double percent = pid.compute(TempSensor::getTemperature(), dtSeconds);
            ValveServo::setTargetPercent(percent);
        } else {
            // Нет валидных данных с датчика - переводим кран в безопасное
            // (минимальное) положение, чтобы не оставлять его открытым
            // "вслепую" при отказе сенсора.
            ValveServo::setTargetPercent(settings.servo.minPercent);
        }
    }

    ValveServo::update();

    if (TempSensor::isValid()) {
        TempHistory::update(TempSensor::getTemperature(), TimeManager::now());
    }

    WebServerManager::update();
    DisplayMenu::update();

    if (settingsSavePending && (now - lastSettingsSaveMs >= SETTINGS_AUTOSAVE_DEBOUNCE_MS)) {
        Storage::save(settings);
        settingsSavePending = false;
        Serial.println(F("[Main] Настройки сохранены в LittleFS"));
    }

    // Явного delay() в loop() нет - это важно для отзывчивости веб-сервера,
    // энкодера и стабильности Wi-Fi стека (аппаратный watchdog ESP8266
    // кормится автоматически на каждой итерации loop()/yield()).
}
