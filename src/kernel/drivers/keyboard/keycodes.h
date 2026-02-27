#ifndef KEYCODES_H
#define KEYCODES_H

/* ====================== */
/* Алфавитно-цифровые клавиши */
/* ====================== */
// Буквы (латинские)
#define KEY_A                   0x1E    // Клавиша 'A'
#define KEY_B                   0x30    // Клавиша 'B'
#define KEY_C                   0x2E    // Клавиша 'C'
#define KEY_D                   0x20    // Клавиша 'D'
#define KEY_E                   0x12    // Клавиша 'E'
#define KEY_F                   0x21    // Клавиша 'F'
#define KEY_G                   0x22    // Клавиша 'G'
#define KEY_H                   0x23    // Клавиша 'H'
#define KEY_I                   0x17    // Клавиша 'I'
#define KEY_J                   0x24    // Клавиша 'J'
#define KEY_K                   0x25    // Клавиша 'K'
#define KEY_L                   0x26    // Клавиша 'L'
#define KEY_M                   0x32    // Клавиша 'M'
#define KEY_N                   0x31    // Клавиша 'N'
#define KEY_O                   0x18    // Клавиша 'O'
#define KEY_P                   0x19    // Клавиша 'P'
#define KEY_Q                   0x10    // Клавиша 'Q'
#define KEY_R                   0x13    // Клавиша 'R'
#define KEY_S                   0x1F    // Клавиша 'S'
#define KEY_T                   0x14    // Клавиша 'T'
#define KEY_U                   0x16    // Клавиша 'U'
#define KEY_V                   0x2F    // Клавиша 'V'
#define KEY_W                   0x11    // Клавиша 'W'
#define KEY_X                   0x2D    // Клавиша 'X'
#define KEY_Y                   0x15    // Клавиша 'Y'
#define KEY_Z                   0x2C    // Клавиша 'Z'

// Цифры (верхний ряд)
#define KEY_1                   0x02    // Клавиша '1' и '!'
#define KEY_2                   0x03    // Клавиша '2' и '@'
#define KEY_3                   0x04    // Клавиша '3' и '#'
#define KEY_4                   0x05    // Клавиша '4' и '$'
#define KEY_5                   0x06    // Клавиша '5' и '%'
#define KEY_6                   0x07    // Клавиша '6' и '^'
#define KEY_7                   0x08    // Клавиша '7' и '&'
#define KEY_8                   0x09    // Клавиша '8' и '*'
#define KEY_9                   0x0A    // Клавиша '9' и '('
#define KEY_0                   0x0B    // Клавиша '0' и ')'

/* ====================== */
/* Символьные клавиши */
/* ====================== */
#define KEY_MINUS               0x0C    // Клавиша '-' и '_'
#define KEY_EQUAL               0x0D    // Клавиша '=' и '+'
#define KEY_LEFT_BRACKET        0x1A    // Клавиша '[' и '{'
#define KEY_RIGHT_BRACKET       0x1B    // Клавиша ']' и '}'
#define KEY_SEMICOLON           0x27    // Клавиша ';' и ':'
#define KEY_APOSTROPHE          0x28    // Клавиша ''' и '"'
#define KEY_BACKTICK            0x29    // Клавиша '`' и '~'
#define KEY_BACKSLASH           0x2B    // Клавиша '\' и '|'
#define KEY_COMMA               0x33    // Клавиша ',' и '<'
#define KEY_PERIOD              0x34    // Клавиша '.' и '>'
#define KEY_SLASH               0x35    // Клавиша '/' и '?'
#define KEY_SPACE               0x39    // Пробел

/* ====================== */
/* Специальные клавиши */
/* ====================== */
#define KEY_ESC                 0x01    // Escape (отмена/выход)
#define KEY_BACKSPACE           0x0E    // Backspace (удаление символа слева)
#define KEY_TAB                 0x0F    // Tab (табуляция)
#define KEY_ENTER               0x1C    // Enter (ввод)
#define KEY_LEFT_SHIFT_PRESS    0x2A    // Левый Shift (нажатие)
#define KEY_RIGHT_SHIFT_PRESS   0x36    // Правый Shift (нажатие)
#define KEY_LEFT_SHIFT_RELEASE  0xAA    // Левый Shift (отпускание)
#define KEY_RIGHT_SHIFT_RELEASE 0xB6    // Правый Shift (отпускание)
#define KEY_LEFT_CTRL_PRESS     0x1D    // Левый Ctrl (нажатие)
#define KEY_LEFT_CTRL_RELEASE   0x9D    // Левый Ctrl (отпускание)
#define KEY_RIGHT_CTRL_PRESS    0xE01D  // Правый Ctrl (нажатие)
#define KEY_RIGHT_CTRL_RELEASE  0xE09D  // Правый Ctrl (отпускание)
#define KEY_LEFT_ALT_PRESS      0x38    // Левый Alt (нажатие)
#define KEY_LEFT_ALT_RELEASE    0xB8    // Левый Alt (отпускание)
#define KEY_RIGHT_ALT_PRESS     0xE038  // Правый Alt/AltGr (нажатие)
#define KEY_RIGHT_ALT_RELEASE   0xE0B8  // Правый Alt/AltGr (отпускание)
#define KEY_CAPS_LOCK           0x3A    // Caps Lock (фиксация заглавных букв)
#define KEY_NUM_LOCK            0x45    // Num Lock (включение цифровой клавиатуры)
#define KEY_SCROLL_LOCK         0x46    // Scroll Lock (режим прокрутки)
#define KEY_LEFT_WIN            0xE05B  // Левая клавиша Windows
#define KEY_RIGHT_WIN           0xE05C  // Правая клавиша Windows
#define KEY_MENU                0xE05D  // Клавиша меню (контекстное меню)
#define KEY_PRINT_SCREEN        0xE037  // Print Screen (снимок экрана)
#define KEY_PAUSE               0xE11D45 // Pause/Break (пауза)

/* ====================== */
/* Функциональные клавиши */
/* ====================== */
#define KEY_F1                  0x3B    // Функциональная клавиша F1
#define KEY_F2                  0x3C    // Функциональная клавиша F2
#define KEY_F3                  0x3D    // Функциональная клавиша F3
#define KEY_F4                  0x3E    // Функциональная клавиша F4
#define KEY_F5                  0x3F    // Функциональная клавиша F5
#define KEY_F6                  0x40    // Функциональная клавиша F6
#define KEY_F7                  0x41    // Функциональная клавиша F7
#define KEY_F8                  0x42    // Функциональная клавиша F8
#define KEY_F9                  0x43    // Функциональная клавиша F9
#define KEY_F10                 0x44    // Функциональная клавиша F10
#define KEY_F11                 0x57    // Функциональная клавиша F11
#define KEY_F12                 0x58    // Функциональная клавиша F12

/* ====================== */
/* Клавиши стрелок */
/* ====================== */
#define KEY_UP_ARROW            0x48    // Стрелка вверх
#define KEY_DOWN_ARROW          0x50    // Стрелка вниз
#define KEY_LEFT_ARROW          0x4B    // Стрелка влево
#define KEY_RIGHT_ARROW         0x4D    // Стрелка вправо

/* ====================== */
/* Цифровая клавиатура */
/* ====================== */
#define KEY_KP_0                0x52    // Цифра 0 на цифровой клавиатуре
#define KEY_KP_1                0x4F    // Цифра 1 на цифровой клавиатуре
#define KEY_KP_2                0x50    // Цифра 2 на цифровой клавиатуре
#define KEY_KP_3                0x51    // Цифра 3 на цифровой клавиатуре
#define KEY_KP_4                0x4B    // Цифра 4 на цифровой клавиатуре
#define KEY_KP_5                0x4C    // Цифра 5 на цифровой клавиатуре
#define KEY_KP_6                0x4D    // Цифра 6 на цифровой клавиатуре
#define KEY_KP_7                0x47    // Цифра 7 на цифровой клавиатуре
#define KEY_KP_8                0x48    // Цифра 8 на цифровой клавиатуре
#define KEY_KP_9                0x49    // Цифра 9 на цифровой клавиатуре
#define KEY_KP_PERIOD           0x53    // Точка (.) на цифровой клавиатуре
#define KEY_KP_PLUS             0x4E    // Плюс (+) на цифровой клавиатуре
#define KEY_KP_MINUS            0x4A    // Минус (-) на цифровой клавиатуре
#define KEY_KP_ASTERISK         0x37    // Звездочка (*) на цифровой клавиатуре
#define KEY_KP_SLASH            0xE035  // Слеш (/) на цифровой клавиатуре
#define KEY_KP_ENTER            0xE01C  // Enter на цифровой клавиатуре

/* ====================== */
/* Дополнительные клавиши */
/* ====================== */
#define KEY_INSERT              0xE052  // Insert (вставка)
#define KEY_DELETE              0xE053  // Delete (удаление)
#define KEY_HOME                0xE047  // Home (в начало)
#define KEY_END                 0xE04F  // End (в конец)
#define KEY_PAGE_UP             0xE049  // Page Up (страница вверх)
#define KEY_PAGE_DOWN           0xE051  // Page Down (страница вниз)

/* ====================== */
/* Мультимедийные клавиши */
/* ====================== */
#define KEY_POWER               0xE05E  // Power (питание)
#define KEY_SLEEP               0xE05F  // Sleep (режим сна)
#define KEY_WAKE                0xE063  // Wake (пробуждение)
#define KEY_VOLUME_UP           0xE030  // Увеличение громкости
#define KEY_VOLUME_DOWN         0xE02E  // Уменьшение громкости
#define KEY_VOLUME_MUTE         0xE020  // Отключение звука
#define KEY_MEDIA_NEXT          0xE019  // Следующий трек
#define KEY_MEDIA_PREV          0xE010  // Предыдущий трек
#define KEY_MEDIA_STOP          0xE024  // Остановка воспроизведения
#define KEY_MEDIA_PLAY_PAUSE    0xE022  // Воспроизведение/пауза

#endif // KEYCODES_H