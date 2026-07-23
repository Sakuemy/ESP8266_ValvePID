#pragma once
/*
 * DisplayMenu.h
 * -----------------------------------------------------------------
 * Локальный интерфейс на OLED-дисплее (SSD1306, I2C) и энкодере с кнопкой.
 * Позволяет посмотреть температуру и IP, а также настроить ПИД, уставку
 * и Wi-Fi (SSID/пароль/режим IP) без веб-интерфейса.
 * -----------------------------------------------------------------
 */

#include "Config.h"

namespace DisplayMenu {

struct Callbacks {
    AppSettings* (*getSettings)();
    void (*onSettingsChanged)(); // сохранить в Storage + применить (ПИД/сервопривод/сеть)
};

void begin(const Callbacks &callbacks);

// Вызывать часто из loop(). Сам поллит энкодер/кнопку и перерисовывает
// экран не чаще, чем нужно.
void update();

} // namespace DisplayMenu
