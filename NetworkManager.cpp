#include "NetworkManager.h"
#include <ESP8266WiFi.h>

namespace NetworkManager {

static NetworkSettings settings_;
static bool apMode_ = false;
static unsigned long connectStartMs_ = 0;
static unsigned long lastRetryMs_ = 0;
static bool connecting_ = false;

static void startStationConnect() {
    WiFi.mode(WIFI_STA);

    if (!settings_.useDhcp && settings_.staticIp != 0) {
        IPAddress ip(settings_.staticIp);
        IPAddress gw(settings_.staticGateway);
        IPAddress sn(settings_.staticSubnet);
        IPAddress dns(settings_.staticDns);
        WiFi.config(ip, gw, sn, dns);
    }

    if (strlen(settings_.ssid) > 0) {
        WiFi.begin(settings_.ssid, settings_.password);
    }
    connectStartMs_ = millis();
    connecting_ = true;
    apMode_ = false;
}

static void startApFallback() {
    Serial.println(F("[Network] Поднимаю точку доступа для первичной настройки"));
    WiFi.mode(WIFI_AP);
    WiFi.softAP(DEFAULT_AP_SSID, DEFAULT_AP_PASS);
    apMode_ = true;
    connecting_ = false;
}

void begin(const NetworkSettings &settings) {
    settings_ = settings;

    if (strlen(settings_.ssid) == 0) {
        // Нет сохранённой сети вовсе - сразу поднимаем AP для первичной настройки
        startApFallback();
        return;
    }

    startStationConnect();
}

void applySettings(const NetworkSettings &settings) {
    settings_ = settings;
    WiFi.disconnect(true);
    delay(100); // короткая пауза допустима - это разовое действие по команде пользователя
    if (strlen(settings_.ssid) == 0) {
        startApFallback();
    } else {
        startStationConnect();
    }
}

void update() {
    if (apMode_) {
        return; // в режиме настройки просто ждём действий пользователя через веб
    }

    if (connecting_) {
        if (WiFi.status() == WL_CONNECTED) {
            connecting_ = false;
            Serial.print(F("[Network] Подключено, IP: "));
            Serial.println(WiFi.localIP());
            return;
        }
        if (millis() - connectStartMs_ > WIFI_CONNECT_TIMEOUT_MS) {
            Serial.println(F("[Network] Таймаут подключения"));
            connecting_ = false;
            lastRetryMs_ = millis();
            // Не поднимаем AP автоматически при временной пропаже сети,
            // чтобы не терять управление краном - просто продолжаем пытаться в фоне.
        }
        return;
    }

    if (WiFi.status() != WL_CONNECTED) {
        if (millis() - lastRetryMs_ > WIFI_RETRY_INTERVAL_MS) {
            Serial.println(F("[Network] Повторная попытка подключения..."));
            lastRetryMs_ = millis();
            startStationConnect();
        }
    }
}

bool isConnected() {
    return !apMode_ && WiFi.status() == WL_CONNECTED;
}

bool isApMode() {
    return apMode_;
}

String getIpAddress() {
    if (apMode_) {
        return WiFi.softAPIP().toString();
    }
    if (WiFi.status() == WL_CONNECTED) {
        return WiFi.localIP().toString();
    }
    return String("0.0.0.0");
}

} // namespace NetworkManager
