#include "Storage.h"
#include <LittleFS.h>
#include <ArduinoJson.h>

namespace Storage {

static void applyDefaults(AppSettings &s) {
    s = AppSettings(); // структуры уже содержат разумные значения по умолчанию
}

bool begin() {
    if (!LittleFS.begin()) {
        Serial.println(F("[Storage] LittleFS.begin() не удался, форматирую..."));
        LittleFS.format();
        return LittleFS.begin();
    }
    return true;
}

bool load(AppSettings &settings) {
    if (!LittleFS.exists(CONFIG_FILE_PATH)) {
        Serial.println(F("[Storage] Файл настроек не найден, создаю значения по умолчанию"));
        applyDefaults(settings);
        save(settings);
        return true;
    }

    File f = LittleFS.open(CONFIG_FILE_PATH, "r");
    if (!f) {
        Serial.println(F("[Storage] Не удалось открыть файл настроек"));
        applyDefaults(settings);
        return false;
    }

    StaticJsonDocument<CONFIG_JSON_CAPACITY> doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();

    if (err) {
        Serial.print(F("[Storage] Ошибка разбора JSON: "));
        Serial.println(err.c_str());
        applyDefaults(settings);
        save(settings);
        return false;
    }

    // ПИД
    settings.pid.kp = doc["pid"]["kp"] | 20.0;
    settings.pid.ki = doc["pid"]["ki"] | 0.5;
    settings.pid.kd = doc["pid"]["kd"] | 1.0;
    settings.pid.setpoint = doc["pid"]["setpoint"] | 60.0;

    // Сервопривод
    settings.servo.minPercent    = doc["servo"]["minPercent"]    | 0;
    settings.servo.maxPercent    = doc["servo"]["maxPercent"]    | 100;
    settings.servo.closedPulseUs = doc["servo"]["closedPulseUs"] | 1000;
    settings.servo.openPulseUs   = doc["servo"]["openPulseUs"]   | 2000;

    // Защита от противоречивых значений в файле настроек (ручное
    // редактирование JSON, повреждение, старая версия прошивки без этой
    // проверки и т.п.) - min не должен быть больше max.
    if (settings.servo.minPercent > settings.servo.maxPercent) {
        uint8_t tmp = settings.servo.minPercent;
        settings.servo.minPercent = settings.servo.maxPercent;
        settings.servo.maxPercent = tmp;
        Serial.println(F("[Storage] servo.minPercent > maxPercent в файле настроек - переставлены местами"));
    }

    // Сеть
    strlcpy(settings.network.ssid, doc["net"]["ssid"] | "", sizeof(settings.network.ssid));
    strlcpy(settings.network.password, doc["net"]["password"] | "", sizeof(settings.network.password));
    settings.network.useDhcp      = doc["net"]["dhcp"] | true;
    settings.network.staticIp      = doc["net"]["ip"]      | 0;
    settings.network.staticGateway = doc["net"]["gateway"] | 0;
    settings.network.staticSubnet  = doc["net"]["subnet"]  | 0;
    settings.network.staticDns     = doc["net"]["dns"]     | 0;

    // Батарея
    settings.battery.voltageAt0Percent   = doc["battery"]["v0"]   | 3.0;
    settings.battery.voltageAt100Percent = doc["battery"]["v100"] | 4.2;
    settings.battery.dividerRatio        = doc["battery"]["dividerRatio"] | BATTERY_DEFAULT_DIVIDER_RATIO;

    // Доступ к настройкам
    strlcpy(settings.admin.password, doc["admin"]["password"] | DEFAULT_ADMIN_PASSWORD, sizeof(settings.admin.password));

    // Дисплей (сон)
    settings.display.sleepEnabled    = doc["display"]["sleepEnabled"]    | true;
    settings.display.sleepTimeoutSec = doc["display"]["sleepTimeoutSec"] | 30;

    return true;
}

bool save(const AppSettings &settings) {
    StaticJsonDocument<CONFIG_JSON_CAPACITY> doc;

    doc["pid"]["kp"] = settings.pid.kp;
    doc["pid"]["ki"] = settings.pid.ki;
    doc["pid"]["kd"] = settings.pid.kd;
    doc["pid"]["setpoint"] = settings.pid.setpoint;

    doc["servo"]["minPercent"]    = settings.servo.minPercent;
    doc["servo"]["maxPercent"]    = settings.servo.maxPercent;
    doc["servo"]["closedPulseUs"] = settings.servo.closedPulseUs;
    doc["servo"]["openPulseUs"]   = settings.servo.openPulseUs;

    doc["net"]["ssid"]     = settings.network.ssid;
    doc["net"]["password"] = settings.network.password;
    doc["net"]["dhcp"]     = settings.network.useDhcp;
    doc["net"]["ip"]       = settings.network.staticIp;
    doc["net"]["gateway"]  = settings.network.staticGateway;
    doc["net"]["subnet"]   = settings.network.staticSubnet;
    doc["net"]["dns"]      = settings.network.staticDns;

    doc["battery"]["v0"]           = settings.battery.voltageAt0Percent;
    doc["battery"]["v100"]         = settings.battery.voltageAt100Percent;
    doc["battery"]["dividerRatio"] = settings.battery.dividerRatio;

    doc["admin"]["password"] = settings.admin.password;

    doc["display"]["sleepEnabled"]    = settings.display.sleepEnabled;
    doc["display"]["sleepTimeoutSec"] = settings.display.sleepTimeoutSec;

    // Пишем во временный файл и затем переименовываем — так при внезапном
    // отключении питания старый рабочий конфиг не будет повреждён.
    const char *tmpPath = "/config.tmp";
    File f = LittleFS.open(tmpPath, "w");
    if (!f) {
        Serial.println(F("[Storage] Не удалось открыть временный файл для записи"));
        return false;
    }

    size_t written = serializeJson(doc, f);
    f.close();

    if (written == 0) {
        Serial.println(F("[Storage] Ошибка сериализации JSON"));
        LittleFS.remove(tmpPath);
        return false;
    }

    LittleFS.remove(CONFIG_FILE_PATH);
    if (!LittleFS.rename(tmpPath, CONFIG_FILE_PATH)) {
        Serial.println(F("[Storage] Не удалось переименовать временный файл"));
        return false;
    }

    return true;
}

} // namespace Storage
