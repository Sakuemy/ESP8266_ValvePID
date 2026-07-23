#include "DisplayMenu.h"
#include <U8g2lib.h>
#include "TempSensor.h"
#include "NetworkManager.h"
#include "BatteryMonitor.h"

namespace DisplayMenu {

// Программный I2C с явно заданными пинами - не зависит от того, на какие
// пины по умолчанию настроен глобальный объект Wire.
static U8G2_SSD1306_128X64_NONAME_F_SW_I2C u8g2(U8G2_R0, /*clock=*/PIN_OLED_SCL, /*data=*/PIN_OLED_SDA, /*reset=*/U8X8_PIN_NONE);

static Callbacks callbacks_;

// ---------------------- Энкодер ----------------------
static int lastClkState_ = HIGH;
static long encoderDelta_ = 0; // накопленные "щелчки" с прошлого чтения

static bool lastButtonState_ = HIGH; // HIGH = не нажата (INPUT_PULLUP)
static unsigned long buttonPressStartMs_ = 0;
static bool buttonHeld_ = false;
static bool clickEvent_ = false;
static bool longPressEvent_ = false;

static void pollEncoder() {
    int clk = digitalRead(PIN_ENC_CLK);
    if (clk != lastClkState_) {
        if (clk == LOW) { // реагируем по фронту спада CLK
            int dt = digitalRead(PIN_ENC_DT);
            encoderDelta_ += (dt != clk) ? 1 : -1;
        }
        lastClkState_ = clk;
    }
}

static void pollButton() {
    clickEvent_ = false;
    longPressEvent_ = false;

    bool pressed = (digitalRead(PIN_ENC_BTN) == LOW);
    unsigned long now = millis();

    if (pressed && !buttonHeld_) {
        buttonHeld_ = true;
        buttonPressStartMs_ = now;
    } else if (pressed && buttonHeld_) {
        if (now - buttonPressStartMs_ > 800UL) {
            longPressEvent_ = true; // будет повторяться, пока кнопка держится - обрабатываем один раз через флаг ниже
        }
    } else if (!pressed && buttonHeld_) {
        buttonHeld_ = false;
        if (now - buttonPressStartMs_ <= 800UL) {
            clickEvent_ = true; // короткий клик
        }
    }
    lastButtonState_ = pressed;
}

// ---------------------- Состояние меню ----------------------
enum class Screen : uint8_t {
    MENU = 0,
    EDIT_KP,
    EDIT_KI,
    EDIT_KD,
    EDIT_SETPOINT,
    EDIT_MIN_PERCENT,
    EDIT_MAX_PERCENT,
    EDIT_BATTERY_V0,
    EDIT_BATTERY_V100,
    EDIT_SSID,
    EDIT_PASSWORD,
    EDIT_DHCP_TOGGLE,
    SAVE_CONFIRM
};

static Screen screen_ = Screen::MENU;
static int menuIndex_ = 0;
static const int MENU_ITEMS_COUNT = 13;
static const char *MENU_LABELS[MENU_ITEMS_COUNT] = {
    "Температура",
    "IP адрес",
    "Батарея",
    "ПИД: Kp",
    "ПИД: Ki",
    "ПИД: Kd",
    "Уставка, C",
    "Мин. открытие %",
    "Макс. открытие %",
    "Батарея: калибр. 0%",
    "Батарея: калибр. 100%",
    "Wi-Fi SSID",
    "Сохранить/выход"
};

// Текстовый редактор (для SSID) - посимвольный выбор через энкодер
static const char CHARSET[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_. ";
static const int CHARSET_LEN = sizeof(CHARSET) - 1;
static char textBuffer_[33];
static int textLen_ = 0;
static int textCharIndex_ = 0; // индекс в CHARSET для текущей позиции курсора

static bool dirty_ = false; // есть несохранённые изменения

static void enterEditNumber(Screen s) {
    screen_ = s;
}

static void enterTextEdit(char *source, size_t maxLen) {
    strlcpy(textBuffer_, source, sizeof(textBuffer_));
    textLen_ = strlen(textBuffer_);
    textCharIndex_ = 0;
    screen_ = Screen::EDIT_SSID;
}

static void applyEncoderToDouble(double &value, double step, double minV, double maxV) {
    if (encoderDelta_ == 0) return;
    value += encoderDelta_ * step;
    if (value < minV) value = minV;
    if (value > maxV) value = maxV;
    encoderDelta_ = 0;
    dirty_ = true;
}

static void applyEncoderToFloat(float &value, float step, float minV, float maxV) {
    if (encoderDelta_ == 0) return;
    value += encoderDelta_ * step;
    if (value < minV) value = minV;
    if (value > maxV) value = maxV;
    encoderDelta_ = 0;
    dirty_ = true;
}

static void applyEncoderToInt(uint8_t &value, int step, int minV, int maxV) {
    if (encoderDelta_ == 0) return;
    int v = (int)value + (int)(encoderDelta_ * step);
    if (v < minV) v = minV;
    if (v > maxV) v = maxV;
    value = (uint8_t)v;
    encoderDelta_ = 0;
    dirty_ = true;
}

// ---------------------- Отрисовка ----------------------
static void drawMenuScreen() {
    u8g2.setFont(u8g2_font_6x10_tr);
    int visibleStart = 0;
    int visibleCount = 5;
    if (menuIndex_ >= visibleCount) visibleStart = menuIndex_ - visibleCount + 1;

    for (int row = 0; row < visibleCount && (visibleStart + row) < MENU_ITEMS_COUNT; row++) {
        int idx = visibleStart + row;
        int y = 10 + row * 11;
        if (idx == menuIndex_) {
            u8g2.drawStr(0, y, ">");
        }

        char line[24];
        if (idx == 0) {
            float t = TempSensor::getTemperature();
            snprintf(line, sizeof(line), "%s: %s", MENU_LABELS[idx],
                     TempSensor::isValid() ? String(t, 1).c_str() : "--");
        } else if (idx == 1) {
            snprintf(line, sizeof(line), "%s", NetworkManager::getIpAddress().c_str());
        } else if (idx == 2) {
            if (BatteryMonitor::isConnected()) {
                snprintf(line, sizeof(line), "Батарея: %d%% %.2fV",
                         (int)BatteryMonitor::getPercent(), BatteryMonitor::getVoltage());
            } else {
                snprintf(line, sizeof(line), "Батарея: не подкл.");
            }
        } else {
            snprintf(line, sizeof(line), "%s", MENU_LABELS[idx]);
        }
        u8g2.drawStr(10, y, line);
    }
}

static void drawNumberEditScreen(const char *label, double value, int decimals) {
    u8g2.setFont(u8g2_font_7x14_tr);
    u8g2.drawStr(0, 16, label);
    char buf[16];
    dtostrf(value, 0, decimals, buf);
    u8g2.setFont(u8g2_font_logisoso20_tn);
    u8g2.drawStr(10, 45, buf);
    u8g2.setFont(u8g2_font_6x10_tr);
    u8g2.drawStr(0, 62, "Клик - сохранить");
}

static void drawTextEditScreen() {
    u8g2.setFont(u8g2_font_6x10_tr);
    u8g2.drawStr(0, 10, "SSID (клик=подтв. симв.)");
    u8g2.drawStr(0, 22, textBuffer_);

    char cur[2] = { CHARSET[textCharIndex_], 0 };
    u8g2.setFont(u8g2_font_logisoso20_tn);
    u8g2.drawStr(50, 50, cur);
    u8g2.setFont(u8g2_font_6x10_tr);
    u8g2.drawStr(0, 62, "Держать - закончить");
}

static void drawSaveConfirmScreen() {
    u8g2.setFont(u8g2_font_7x14_tr);
    u8g2.drawStr(0, 25, "Сохранить настройки?");
    u8g2.setFont(u8g2_font_6x10_tr);
    u8g2.drawStr(0, 45, "Клик - да, держать - отмена");
}

// ---------------------- Обработка ввода по экранам ----------------------
static void handleMenuScreen(AppSettings *s) {
    if (encoderDelta_ != 0) {
        menuIndex_ += (encoderDelta_ > 0) ? 1 : -1;
        if (menuIndex_ < 0) menuIndex_ = 0;
        if (menuIndex_ >= MENU_ITEMS_COUNT) menuIndex_ = MENU_ITEMS_COUNT - 1;
        encoderDelta_ = 0;
    }

    if (!clickEvent_) return;

    switch (menuIndex_) {
        case 0: case 1: case 2: break; // только просмотр
        case 3: enterEditNumber(Screen::EDIT_KP); break;
        case 4: enterEditNumber(Screen::EDIT_KI); break;
        case 5: enterEditNumber(Screen::EDIT_KD); break;
        case 6: enterEditNumber(Screen::EDIT_SETPOINT); break;
        case 7: enterEditNumber(Screen::EDIT_MIN_PERCENT); break;
        case 8: enterEditNumber(Screen::EDIT_MAX_PERCENT); break;
        case 9: enterEditNumber(Screen::EDIT_BATTERY_V0); break;
        case 10: enterEditNumber(Screen::EDIT_BATTERY_V100); break;
        case 11: enterTextEdit(s->network.ssid, sizeof(s->network.ssid)); break;
        case 12: screen_ = Screen::SAVE_CONFIRM; break;
    }
}

static void handleNumberEditScreen(AppSettings *s) {
    switch (screen_) {
        case Screen::EDIT_KP: applyEncoderToDouble(s->pid.kp, 0.1, 0.0, 1000.0); break;
        case Screen::EDIT_KI: applyEncoderToDouble(s->pid.ki, 0.01, 0.0, 100.0); break;
        case Screen::EDIT_KD: applyEncoderToDouble(s->pid.kd, 0.01, 0.0, 100.0); break;
        case Screen::EDIT_SETPOINT: applyEncoderToDouble(s->pid.setpoint, 0.5, 0.0, 150.0); break;
        case Screen::EDIT_MIN_PERCENT: applyEncoderToInt(s->servo.minPercent, 1, 0, 100); break;
        case Screen::EDIT_MAX_PERCENT: applyEncoderToInt(s->servo.maxPercent, 1, 0, 100); break;
        case Screen::EDIT_BATTERY_V0: applyEncoderToFloat(s->battery.voltageAt0Percent, 0.05f, 0.0f, 60.0f); break;
        case Screen::EDIT_BATTERY_V100: applyEncoderToFloat(s->battery.voltageAt100Percent, 0.05f, 0.0f, 60.0f); break;
        default: break;
    }

    if (clickEvent_) {
        screen_ = Screen::MENU;
    }
}

static void handleTextEditScreen(AppSettings *s) {
    if (encoderDelta_ != 0) {
        textCharIndex_ = (textCharIndex_ + (int)encoderDelta_) % CHARSET_LEN;
        if (textCharIndex_ < 0) textCharIndex_ += CHARSET_LEN;
        encoderDelta_ = 0;
    }

    if (clickEvent_) {
        if (textLen_ < (int)sizeof(textBuffer_) - 1) {
            textBuffer_[textLen_++] = CHARSET[textCharIndex_];
            textBuffer_[textLen_] = '\0';
        }
    }

    if (longPressEvent_) {
        strlcpy(s->network.ssid, textBuffer_, sizeof(s->network.ssid));
        dirty_ = true;
        screen_ = Screen::MENU;
    }
}

static void handleSaveConfirmScreen(AppSettings *s) {
    if (clickEvent_) {
        if (callbacks_.onSettingsChanged) callbacks_.onSettingsChanged();
        dirty_ = false;
        screen_ = Screen::MENU;
    }
    if (longPressEvent_) {
        screen_ = Screen::MENU; // отмена, без сохранения (изменения в оперативных полях останутся до перезагрузки)
    }
}

// ---------------------- Основной цикл ----------------------
static unsigned long lastRenderMs_ = 0;

void begin(const Callbacks &callbacks) {
    callbacks_ = callbacks;
    pinMode(PIN_ENC_CLK, INPUT_PULLUP);
    pinMode(PIN_ENC_DT, INPUT_PULLUP);
    pinMode(PIN_ENC_BTN, INPUT_PULLUP);
    lastClkState_ = digitalRead(PIN_ENC_CLK);

    u8g2.begin();
    u8g2.setContrast(180);
}

void update() {
    pollEncoder();
    pollButton();

    AppSettings *s = callbacks_.getSettings();

    switch (screen_) {
        case Screen::MENU: handleMenuScreen(s); break;
        case Screen::EDIT_SSID: handleTextEditScreen(s); break;
        case Screen::SAVE_CONFIRM: handleSaveConfirmScreen(s); break;
        default: handleNumberEditScreen(s); break;
    }

    // Перерисовываем максимум ~10 раз в секунду - этого достаточно для
    // отзывчивого UI и не грузит процессор лишний раз.
    unsigned long now = millis();
    if (now - lastRenderMs_ < 100UL) return;
    lastRenderMs_ = now;

    u8g2.clearBuffer();
    switch (screen_) {
        case Screen::MENU: drawMenuScreen(); break;
        case Screen::EDIT_KP: drawNumberEditScreen("Kp", s->pid.kp, 2); break;
        case Screen::EDIT_KI: drawNumberEditScreen("Ki", s->pid.ki, 2); break;
        case Screen::EDIT_KD: drawNumberEditScreen("Kd", s->pid.kd, 2); break;
        case Screen::EDIT_SETPOINT: drawNumberEditScreen("Уставка, C", s->pid.setpoint, 1); break;
        case Screen::EDIT_MIN_PERCENT: drawNumberEditScreen("Мин %", s->servo.minPercent, 0); break;
        case Screen::EDIT_MAX_PERCENT: drawNumberEditScreen("Макс %", s->servo.maxPercent, 0); break;
        case Screen::EDIT_BATTERY_V0: drawNumberEditScreen("Батарея 0%, В", s->battery.voltageAt0Percent, 2); break;
        case Screen::EDIT_BATTERY_V100: drawNumberEditScreen("Батарея 100%, В", s->battery.voltageAt100Percent, 2); break;
        case Screen::EDIT_SSID: drawTextEditScreen(); break;
        case Screen::SAVE_CONFIRM: drawSaveConfirmScreen(); break;
        default: break;
    }
    u8g2.sendBuffer();
}

} // namespace DisplayMenu
