#pragma once
/*
 * WebServerManager.h
 * -----------------------------------------------------------------
 * Встроенный веб-интерфейс:
 *   GET  /                    - дашборд (температура, % открытия, график 24ч)
 *   GET  /settings            - страница настроек ПИД / сервопривода / сети
 *   GET  /api/status          - JSON текущего состояния (поллинг раз в секунду)
 *   GET  /api/history         - JSON истории температуры за 24ч
 *   GET  /api/settings        - JSON текущих настроек (для заполнения формы)
 *   POST /api/settings/pid    - обновление параметров ПИД
 *   POST /api/settings/servo  - обновление лимитов/калибровки сервопривода
 *   POST /api/settings/network- обновление сетевых настроек (SSID/пароль/IP)
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
