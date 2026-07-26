/*
 * ESP8266_ValvePID.ino
 * =====================================================================
 * ПИД-регулятор температуры для шарового крана на сервоприводе, ESP8266.
 *
 * Функционал:
 *  - Чтение температуры DS18B20 (неблокирующее).
 *  - ПИД-регулирование % открытия крана, плавный ход сервопривода.
 *  - Тестовый ручной поворот крана на угол (меню на дисплее).
 *  - Веб-интерфейс: мониторинг (график 24ч), настройки ПИД/сервопривода/
 *    Wi-Fi/батареи/часового пояса.
 *  - Локальное меню на OLED + энкодере, с автогашением (сон) экрана.
 *  - NTP-синхронизация времени с оффлайн-фолбэком и настраиваемым
 *    часовым поясом.
 *  - Все пользовательские настройки хранятся в LittleFS (JSON); история
 *    температуры живёт только в RAM (не переживает перезагрузку
 *    устройства, но переживает перезагрузку страницы в браузере).
 *  - Пароль администратора хранится только в виде SHA-256 хэша с солью.
 *  - Безопасные положения крана: при потере датчика температуры или
 *    критическом разряде батареи кран принудительно закрывается.
 *  - OTA-обновление прошивки по Wi-Fi (ArduinoOTA).
 *
 * Требуемые библиотеки (Arduino Library Manager):
 *  - OneWire
 *  - DallasTemperature
 *  - ArduinoJson (v6.x)
 *  - U8g2
 *  (Servo, ESP8266WiFi, ESP8266WebServer, LittleFS, ArduinoOTA, base64,
 *   BearSSL идут в комплекте с ESP8266 core для Arduino - отдельно
 *   устанавливать не нужно)
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
#include <ArduinoOTA.h> // входит в состав ESP8266 core, доп. библиотека не нужна

static AppSettings settings;
static PIDController pid;

static unsigned long lastPidComputeMs = 0;
static unsigned long lastSettingsSaveMs = 0;
static bool settingsSavePending = false;
static bool batteryCriticalLogged = false; // чтобы не спамить Serial каждый цикл

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
    TimeManager::applySettings(settings.time.gmtOffsetSec);

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
    TimeManager::begin(settings.time.gmtOffsetSec);

    pid.configure(settings.pid.kp, settings.pid.ki, settings.pid.kd, settings.pid.setpoint);

    NetworkManager::begin(settings.network);

    // OTA (обновление прошивки по Wi-Fi) - удобно, когда устройство уже
    // установлено на месте (у крана/котла) и снимать его для прошивки по
    // USB неудобно. Работает, как только Wi-Fi подключится (в STA-режиме;
    // в режиме AP настройки недоступен, что ожидаемо).
    ArduinoOTA.setHostname(OTA_HOSTNAME);
    ArduinoOTA.setPassword(OTA_PASSWORD);
    ArduinoOTA.onStart([]() {
        Serial.println(F("[OTA] Начало обновления прошивки..."));
    });
    ArduinoOTA.onEnd([]() {
        Serial.println(F("[OTA] Обновление завершено, перезагрузка..."));
    });
    ArduinoOTA.onError([](ota_error_t error) {
        Serial.printf("[OTA] Ошибка (%u)\n", (unsigned)error);
    });
    ArduinoOTA.begin();

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
    ArduinoOTA.handle();
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

    // Критически низкий заряд батареи (если она вообще физически
    // подключена - см. BatteryMonitor::isConnected()) - принудительно
    // держим кран в минимальном (безопасном) положении, перекрывая то,
    // что мог выставить ПИД в этом цикле. Проверяем и применяем ДО
    // ValveServo::update(), чтобы сработало в этом же такте, а не с
    // задержкой до следующего. Ничего не сохраняем и не блокируем - как
    // только заряд восстановится (или батарею зарядят/заменят), обычная
    // логика вернётся сама, без перезагрузки.
    bool batteryCritical = BatteryMonitor::isConnected() &&
                            BatteryMonitor::getPercent() < BATTERY_CRITICAL_PERCENT;
    if (batteryCritical) {
        ValveServo::setTargetPercent(settings.servo.minPercent);
        if (!batteryCriticalLogged) {
            Serial.println(F("[Main] Критически низкий заряд батареи - кран переведён в безопасное положение"));
            batteryCriticalLogged = true;
        }
    } else {
        batteryCriticalLogged = false;
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
