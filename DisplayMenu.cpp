#include "DisplayMenu.h"
#include <U8g2lib.h>
#include "TempSensor.h"
#include "NetworkManager.h"
#include "BatteryMonitor.h"

namespace DisplayMenu {

// Программный I2C с явно заданными пинами - не зависит от того, на какие
// пины по умолчанию настроен глобальный объект Wire.
static U8G2_SSD1306_128X64_NONAME_F_SW_I2C u8g2(U8G2_R0, /*clock=*/PIN_OLED_SCL, /*data=*/PIN_OLED_SDA, /*reset=*/U8X8_PIN_NONE);
static bool displayOk_ = false; // true, если дисплей отозвался при инициализации

static Callbacks callbacks_;

// ---------------------- Энкодер (с гашением дребезга) ----------------------
// CLK/DT читаются поллингом (не по прерыванию). Само по себе сравнение
// "изменилось/не изменилось" на каждом вызове update() ловит и дребезг
// контактов на фронтах - на нём один физический "щелчок" энкодера может
// породить несколько ложных срабатываний. Поэтому изменение уровня CLK
// принимается как достоверное только после того, как он удержался
// стабильным не менее ENCODER_DEBOUNCE_MS.
static int lastClkReading_ = HIGH; // последнее "сырое" чтение пина
static int lastClkStable_  = HIGH; // последнее принятое (отфильтрованное) состояние
static unsigned long lastClkChangeMs_ = 0;
static long encoderDelta_ = 0; // накопленные "щелчки" с прошлого чтения

// ---------------------- Кнопка (с гашением дребезга) ----------------------
static int lastButtonReading_ = HIGH; // HIGH = не нажата (INPUT_PULLUP)
static int buttonStable_ = HIGH;
static unsigned long lastButtonChangeMs_ = 0;
static unsigned long buttonPressStartMs_ = 0;
static bool buttonHeld_ = false;
static bool clickEvent_ = false;
static bool longPressEvent_ = false;

static void pollEncoder() {
    unsigned long now = millis();
    int reading = digitalRead(PIN_ENC_CLK);

    if (reading != lastClkReading_) {
        // Уровень изменился по сравнению с прошлым чтением - запоминаем
        // момент и ждём, не "задребезжит" ли он обратно.
        lastClkChangeMs_ = now;
        lastClkReading_ = reading;
    }

    if ((now - lastClkChangeMs_) >= ENCODER_DEBOUNCE_MS && reading != lastClkStable_) {
        // Уровень удержался стабильным дольше дебаунса - принимаем как
        // настоящий фронт.
        lastClkStable_ = reading;
        if (reading == LOW) { // реагируем по фронту спада CLK
            int dt = digitalRead(PIN_ENC_DT);
            encoderDelta_ += (dt != reading) ? 1 : -1;
        }
    }
}

static void pollButton() {
    clickEvent_ = false;
    longPressEvent_ = false;

    unsigned long now = millis();
    int reading = digitalRead(PIN_ENC_BTN);

    if (reading != lastButtonReading_) {
        lastButtonChangeMs_ = now;
        lastButtonReading_ = reading;
    }

    if ((now - lastButtonChangeMs_) >= BUTTON_DEBOUNCE_MS && reading != buttonStable_) {
        buttonStable_ = reading;
        bool pressed = (buttonStable_ == LOW);
        if (pressed) {
            buttonHeld_ = true;
            buttonPressStartMs_ = now;
        } else if (buttonHeld_) {
            buttonHeld_ = false;
            if (now - buttonPressStartMs_ <= 800UL) {
                clickEvent_ = true; // короткий клик
            }
        }
    }

    // Долгое удержание проверяем независимо от дебаунс-таймера смены
    // состояния (кнопка уже стабильно нажата какое-то время).
    if (buttonHeld_ && (now - buttonPressStartMs_ > 800UL)) {
        longPressEvent_ = true; // будет повторяться, пока кнопка держится - обрабатывающий код читает флаг один раз за цикл
    }
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
    EDIT_BATTERY_DIVIDER,
    EDIT_SSID,
    EDIT_ADMIN_PASSWORD,
    ENTER_PASSWORD,
    EDIT_DHCP_TOGGLE,
    SAVE_CONFIRM
};

static Screen screen_ = Screen::MENU;
static int menuIndex_ = 0;
static const int MENU_ITEMS_COUNT = 15;
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
    "Батарея: коэф. делителя",
    "Wi-Fi SSID",
    "Сменить пароль",
    "Сохранить/выход"
};

// Пункты меню с индексом >= FIRST_PROTECTED_ITEM изменяют настройки и
// требуют предварительной разблокировки паролем (см. unlocked_ ниже).
// Пункты 0..2 - это просмотр статуса, доступный всегда без пароля.
static const int FIRST_PROTECTED_ITEM = 3;

// Текстовый редактор (для SSID/пароля) - посимвольный выбор через энкодер
static const char CHARSET[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_. ";
static const int CHARSET_LEN = sizeof(CHARSET) - 1;
static char textBuffer_[33];
static int textLen_ = 0;
static int textCharIndex_ = 0; // индекс в CHARSET для текущей позиции курсора

// К какому полю применить результат текстового редактора при подтверждении.
enum class TextEditTarget : uint8_t { NONE, SSID, ADMIN_PASSWORD, LOGIN_CHECK };
static TextEditTarget textEditTarget_ = TextEditTarget::NONE;

static bool dirty_ = false; // есть несохранённые изменения

// ---------------------- Доступ по паролю ----------------------
// Разблокировка живёт только в оперативной памяти на время сессии работы
// с меню и автоматически "гаснет" после MENU_LOCK_TIMEOUT_MS бездействия -
// так же, как разлогинивание по таймауту в веб-интерфейсе.
static bool unlocked_ = false;
static unsigned long lastInteractionMs_ = 0;
static int pendingMenuIndex_ = -1; // пункт меню, куда перейти после успешного ввода пароля
static bool wrongPassword_ = false;
static unsigned long wrongPasswordShownMs_ = 0;

static void enterMenuItem(int idx, AppSettings *s) {
    switch (idx) {
        case 3: screen_ = Screen::EDIT_KP; break;
        case 4: screen_ = Screen::EDIT_KI; break;
        case 5: screen_ = Screen::EDIT_KD; break;
        case 6: screen_ = Screen::EDIT_SETPOINT; break;
        case 7: screen_ = Screen::EDIT_MIN_PERCENT; break;
        case 8: screen_ = Screen::EDIT_MAX_PERCENT; break;
        case 9: screen_ = Screen::EDIT_BATTERY_V0; break;
        case 10: screen_ = Screen::EDIT_BATTERY_V100; break;
        case 11: screen_ = Screen::EDIT_BATTERY_DIVIDER; break;
        case 12:
            strlcpy(textBuffer_, s->network.ssid, sizeof(textBuffer_));
            textLen_ = strlen(textBuffer_);
            textCharIndex_ = 0;
            textEditTarget_ = TextEditTarget::SSID;
            screen_ = Screen::EDIT_SSID;
            break;
        case 13:
            textBuffer_[0] = '\0';
            textLen_ = 0;
            textCharIndex_ = 0;
            textEditTarget_ = TextEditTarget::ADMIN_PASSWORD;
            screen_ = Screen::EDIT_SSID; // используем тот же экран-редактор текста
            break;
        case 14: screen_ = Screen::SAVE_CONFIRM; break;
        default: screen_ = Screen::MENU; break;
    }
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
    u8g2.drawStr(100, 8, unlocked_ ? "[откр]" : "[закр]");

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
    const char *title = "SSID (клик=подтв. симв.)";
    if (textEditTarget_ == TextEditTarget::ADMIN_PASSWORD) title = "Новый пароль";
    else if (textEditTarget_ == TextEditTarget::LOGIN_CHECK) title = "Пароль (для входа)";
    u8g2.drawStr(0, 10, title);

    // Пароль не отображаем открытым текстом на экране - только звёздочки
    // и длину, чтобы его нельзя было подсмотреть через плечо.
    if (textEditTarget_ == TextEditTarget::ADMIN_PASSWORD || textEditTarget_ == TextEditTarget::LOGIN_CHECK) {
        char masked[34];
        int n = textLen_;
        if (n > 33) n = 33;
        for (int i = 0; i < n; i++) masked[i] = '*';
        masked[n] = '\0';
        u8g2.drawStr(0, 22, masked);
    } else {
        u8g2.drawStr(0, 22, textBuffer_);
    }

    char cur[2] = { CHARSET[textCharIndex_], 0 };
    u8g2.setFont(u8g2_font_logisoso20_tn);
    u8g2.drawStr(50, 50, cur);
    u8g2.setFont(u8g2_font_6x10_tr);
    u8g2.drawStr(0, 62, "Держать - закончить");
}

static void drawEnterPasswordScreen() {
    u8g2.setFont(u8g2_font_7x14_tr);
    u8g2.drawStr(0, 14, "Введите пароль");

    char masked[34];
    int n = textLen_;
    if (n > 33) n = 33;
    for (int i = 0; i < n; i++) masked[i] = '*';
    masked[n] = '\0';
    u8g2.setFont(u8g2_font_6x10_tr);
    u8g2.drawStr(0, 26, masked);

    char cur[2] = { CHARSET[textCharIndex_], 0 };
    u8g2.setFont(u8g2_font_logisoso20_tn);
    u8g2.drawStr(50, 50, cur);
    u8g2.setFont(u8g2_font_6x10_tr);
    if (wrongPassword_) {
        u8g2.drawStr(0, 62, "Неверный пароль!");
    } else {
        u8g2.drawStr(0, 62, "Клик=симв. Держать=ОК");
    }
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

    if (menuIndex_ < FIRST_PROTECTED_ITEM) return; // просмотровые пункты - без пароля

    if (!unlocked_) {
        // Запоминаем, куда хотели попасть, и просим пароль. После успешного
        // ввода перейдём сразу в нужный пункт, не заставляя выбирать заново.
        pendingMenuIndex_ = menuIndex_;
        textBuffer_[0] = '\0';
        textLen_ = 0;
        textCharIndex_ = 0;
        textEditTarget_ = TextEditTarget::LOGIN_CHECK;
        wrongPassword_ = false;
        screen_ = Screen::ENTER_PASSWORD;
        return;
    }

    enterMenuItem(menuIndex_, s);
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
        case Screen::EDIT_BATTERY_DIVIDER: applyEncoderToFloat(s->battery.dividerRatio, 0.01f, 1.0f, 50.0f); break;
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
        if (textEditTarget_ == TextEditTarget::SSID) {
            strlcpy(s->network.ssid, textBuffer_, sizeof(s->network.ssid));
            dirty_ = true;
        } else if (textEditTarget_ == TextEditTarget::ADMIN_PASSWORD) {
            if (textLen_ >= 4) { // минимальная длина, как и в веб-форме
                strlcpy(s->admin.password, textBuffer_, sizeof(s->admin.password));
                dirty_ = true;
            }
        }
        textEditTarget_ = TextEditTarget::NONE;
        screen_ = Screen::MENU;
    }
}

static void handleEnterPasswordScreen(AppSettings *s) {
    if (encoderDelta_ != 0) {
        textCharIndex_ = (textCharIndex_ + (int)encoderDelta_) % CHARSET_LEN;
        if (textCharIndex_ < 0) textCharIndex_ += CHARSET_LEN;
        encoderDelta_ = 0;
        wrongPassword_ = false;
    }

    if (clickEvent_) {
        if (textLen_ < (int)sizeof(textBuffer_) - 1) {
            textBuffer_[textLen_++] = CHARSET[textCharIndex_];
            textBuffer_[textLen_] = '\0';
        }
        wrongPassword_ = false;
    }

    if (longPressEvent_) {
        if (strcmp(textBuffer_, s->admin.password) == 0) {
            unlocked_ = true;
            wrongPassword_ = false;
            lastInteractionMs_ = millis();
            int target = pendingMenuIndex_;
            pendingMenuIndex_ = -1;
            screen_ = Screen::MENU;
            if (target >= FIRST_PROTECTED_ITEM) {
                enterMenuItem(target, s);
            }
        } else {
            // Неверный пароль: не блокируем устройство навсегда, просто
            // очищаем ввод и показываем сообщение об ошибке ненадолго.
            wrongPassword_ = true;
            wrongPasswordShownMs_ = millis();
            textBuffer_[0] = '\0';
            textLen_ = 0;
            textCharIndex_ = 0;
        }
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
    lastClkReading_ = digitalRead(PIN_ENC_CLK);
    lastClkStable_ = lastClkReading_;
    lastButtonReading_ = digitalRead(PIN_ENC_BTN);
    buttonStable_ = lastButtonReading_;

    // u8g2.begin() возвращает true/false в зависимости от версии библиотеки
    // не во всех вариантах - но I2C-транзакция внутри него завершится
    // ошибкой, если экран не отвечает (неверный адрес/не подключен/не
    // запитан). Логируем результат в Serial, чтобы при "не работает экран"
    // можно было быстро понять - это проблема прошивки/пинов или сама
    // физическая линия I2C.
    displayOk_ = u8g2.begin();
    if (!displayOk_) {
        Serial.println(F("[Display] u8g2.begin() не подтвердил инициализацию дисплея."));
        Serial.println(F("[Display] Проверьте: питание OLED (3.3В), SDA=D5(GPIO14) SCL=D6(GPIO12),"));
        Serial.println(F("[Display] а также адрес I2C платы (по умолчанию 0x3C - см. README, если у вас модуль на 0x3D)."));
    } else {
        Serial.println(F("[Display] OLED инициализирован"));
    }

    u8g2.setContrast(180);
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_6x10_tr);
    u8g2.drawStr(0, 20, "Valve PID");
    u8g2.drawStr(0, 34, displayOk_ ? "Инициализация..." : "Дисплей не отвечает?");
    u8g2.sendBuffer();
}

void update() {
    pollEncoder();
    pollButton();

    AppSettings *s = callbacks_.getSettings();

    // Автоблокировка: если меню было разблокировано, но им какое-то время
    // не пользовались, требуем пароль заново - как разлогинивание веб-сессии
    // по таймауту. Взаимодействием считаем любое вращение энкодера или клик.
    if (encoderDelta_ != 0 || clickEvent_ || longPressEvent_) {
        lastInteractionMs_ = millis();
    } else if (unlocked_ && (millis() - lastInteractionMs_ > MENU_LOCK_TIMEOUT_MS)) {
        unlocked_ = false;
        if (screen_ != Screen::MENU) screen_ = Screen::MENU;
    }

    switch (screen_) {
        case Screen::MENU: handleMenuScreen(s); break;
        case Screen::EDIT_SSID: handleTextEditScreen(s); break;
        case Screen::ENTER_PASSWORD: handleEnterPasswordScreen(s); break;
        case Screen::SAVE_CONFIRM: handleSaveConfirmScreen(s); break;
        default: handleNumberEditScreen(s); break;
    }

    // Перерисовываем максимум ~10 раз в секунду - этого достаточно для
    // отзывчивого UI и не грузит процессор лишний раз.
    unsigned long now = millis();
    if (now - lastRenderMs_ < 100UL) return;
    lastRenderMs_ = now;

    // Сообщение "неверный пароль" само гаснет через 1.5с.
    if (wrongPassword_ && (now - wrongPasswordShownMs_ > 1500UL)) {
        wrongPassword_ = false;
    }

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
        case Screen::EDIT_BATTERY_DIVIDER: drawNumberEditScreen("Коэф. делителя", s->battery.dividerRatio, 3); break;
        case Screen::EDIT_SSID: drawTextEditScreen(); break;
        case Screen::ENTER_PASSWORD: drawEnterPasswordScreen(); break;
        case Screen::SAVE_CONFIRM: drawSaveConfirmScreen(); break;
        default: break;
    }
    u8g2.sendBuffer();
}

} // namespace DisplayMenu
