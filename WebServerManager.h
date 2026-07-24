#pragma once
/*
 * WebServerManager.h
 * -----------------------------------------------------------------
 * Встроенный веб-интерфейс:
 *   GET  /                     - дашборд (температура, % открытия, график 24ч), без пароля
 *   GET  /settings             - настройки ПИД/сервопривода/сети/батареи, требует пароль (HTTP Basic Auth)
 *   GET  /api/status           - JSON текущего состояния (поллинг раз в секунду), без пароля
 *   GET  /api/history          - JSON истории температуры за 24ч, без пароля
 *   GET  /api/settings         - JSON текущих настроек (для заполнения формы), требует пароль
 *   POST /api/settings/pid     - обновление параметров ПИД, требует пароль
 *   POST /api/settings/servo   - обновление лимитов/калибровки сервопривода, требует пароль
 *   POST /api/settings/battery - калибровка батареи + коэфф. делителя A0, требует пароль
 *   POST /api/settings/network - обновление сетевых настроек (SSID/пароль/IP), требует пароль
 *   POST /api/settings/security- смена пароля доступа (текущий+новый), требует пароль
 *
 * Пароль - HTTP Basic Auth, логин фиксирован (ADMIN_USERNAME в Config.h),
 * сам пароль хранится в settings.admin.password (по умолчанию см.
 * DEFAULT_ADMIN_PASSWORD в Config.h - смените при первом запуске).
 * -----------------------------------------------------------------
 */

#include "Config.h"

namespace WebServerManager {

// Функции обратного вызова, через которые веб-сервер получает/меняет
// общее состояние приложения, не зная деталей других модулей.
struct Callbacks {
    AppSettings* (*getSettings)();
    void (*onSettingsChanged)(); // вызывается после того, как настройки изменены и сохранены
};

void begin(const Callbacks &callbacks);
void update(); // вызывать часто из loop() - server.handleClient()

} // namespace WebServerManager
