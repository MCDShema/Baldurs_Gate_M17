# 🗡️ Baldur's Gate: The Original Saga для консолі M17

[![Platform](https://img.shields.io/badge/Platform-M17%20Handheld-blue.svg)](https://github.com/MCDShema/Baldurs_Gate_M17)
[![Engine](https://img.shields.io/badge/Engine-GemRB%20ARM-darkblue.svg)](https://github.com/gemrb/gemrb)
[![Status](https://img.shields.io/badge/Status-Port%20Configuration%20%26%20Mapper-orange.svg)](https://github.com/MCDShema/Baldurs_Gate_M17)

Пакет файлів, скриптів середовища, конфігурації та драйвера керування для запуску культової RPG **Baldur's Gate: The Original Saga** (включно з доповненням Tales of the Sword Coast) на портативній консолі **M17** за допомогою рушія **GemRB**.

---

### 📥 Швидке завантаження
👉 **[Завантажити архів порту (ZIP)](https://github.com/MCDShema/Baldurs_Gate_M17/archive/refs/heads/main.zip)**

---

## 🛠️ Що входить до складу порту

1. **`Baldurs_Gate.sh`**:
   - Автоматичне визначення точки монтування картки пам'яті (`/storage`, `/sdcard`, `/mnt/sdcard`).
   - Перевірка наявності відеоприскорювача Rockchip Mali (`/dev/mali0`, `/dev/fb0`), прав доступу та конфігурації DirectFB/OpenGL ES.
   - Завантаження бібліотеки сумісності `libsdl_compat.so` та передача параметрів рушію.
   - Автоматичний запуск і завершення фонового маппера кнопок.
2. **`m17_gemrb_mapper`** (+ вихідний код `m17_bg_mapper.c`):
   - Спеціальний апаратний маппер фізичних кнопок та стіка консолі M17 у рух віртуальної миші та клавіатурні скорочення Infinity Engine (вибір всієї партії, пауза, швидке збереження, інвентар).
3. **`GemRB.cfg`**:
   - Повністю підготовлена конфігурація під екран консолі M17 (роздільна здатність 640x480, шрифти, чутливість покажчика).
4. **`GUIScripts/` та `unhardcoded/`**:
   - Адаптовані Python/GUI-скрипти інтерфейсу та системні ресурси рушія GemRB.
5. **`games/bg1/override/`**:
   - Патчі інтерфейсу та шрифтів для коректного відображення на компактному екрані.

---

## 📦 Як встановити та додати файли гри

1. **Завантажте та розархівуйте цей репозиторій**:
   - Скопіюйте папку `roms` на вашу картку пам'яті M17 (файли потраплять у `roms/ports/`).
2. **Додайте ресурси оригінальної гри Baldur's Gate 1 (GOG / ISO / Steam)**:
   - Для повноцінної роботи покладіть файли гри у папку на флешці:
     `roms/ports/gemrb/games/bg1/`
   - Обов'язкові файли:
     - `chitin.key`
     - `dialog.tlk`
     - папка `data/` (з усіма файлами `.bif`)
3. **Бінарний файл рушія**:
   - Переконайтеся, що ARM-бінарник `gemrb` розміщено у `roms/ports/gemrb/gemrb`.
4. **Запуск**:
   - Вставте картку в консоль M17, відкрийте меню **PORTS** та запустіть **Baldur's Gate**.

---

## 📁 Структура файлів на картці пам'яті

```text
/roms/ports/
├── Baldurs_Gate.sh                   # Головний скрипт запуску гри
└── gemrb/                            # Каталог рушія та конфігурації
    ├── GemRB.cfg                     # Конфігурація гри
    ├── m17_gemrb_mapper              # Драйвер маппінгу кнопок M17
    ├── m17_bg_mapper.c               # Вихідний код маппера
    ├── libs/
    │   └── libsdl_compat.so          # Бібліотека сумісності SDL2
    ├── GUIScripts/                   # Скрипти інтерфейсу
    ├── unhardcoded/                  # Системні ресурси GemRB
    └── games/
        └── bg1/                      # Каталог для ресурсів BG1 (chitin.key, data/)
            ├── baldur.ini
            └── override/
```
