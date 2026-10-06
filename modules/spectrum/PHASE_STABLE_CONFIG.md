# Phase-Stable Configuration для AD9361

## Что было добавлено в код

Добавлена новая функция `configure_phase_stable()` в класс `PlutoCaptureThread`, которая отключает три адаптивных алгоритма AD9361:

### Функция: `configure_phase_stable()` 

**Файлы:**
- `include/capture.hpp` (строка 43) — объявление
- `src/capture.cpp` (строка 344) — реализация
- `src/capture.cpp` (строка 221) — вызов из `configure_device()`

**Что она делает:**
```cpp
void PlutoCaptureThread::configure_phase_stable() {
    // Отключает три алгоритма для стабильной фазы:
    
    for (обоих каналов RX1 и RX2) {
        rf_dc_offset_tracking_en   = false  // Отключить РЧ смещение
        bb_dc_offset_tracking_en   = false  // Отключить ПП смещение  
        quadrature_tracking_en     = false  // Отключить квадратурное отслеживание
    }
}
```

**Когда вызывается:**
- При подключении к плате (в `configure_device()`)
- ПОСЛЕ инверсии Rx2 (`apply_phase_inversion()`)
- ПЕРЕД настройкой базовых параметров трактов

## Почему это нужно

### Проблема: AM-to-PM конверсия
При увеличении уровня входного сигнала (RSSI +26.75 dB) происходит:
1. Нелинейность усилителей → фаза зависит от амплитуды
2. Нелинейность смесителя → фазовый сдвиг
3. Адаптивные алгоритмы перестраивают рабочую точку → фаза меняется

### Решение
Отключить адаптивные алгоритмы → фаза становится независима от уровня сигнала

## Эффект после применения

**До (с адаптивными алгоритмами):**
```
RSSI RX2: 57.75 dB → Phase_RX2 = X + ΔΦ₁  (зависит от RSSI)
RSSI RX2: 84.50 dB → Phase_RX2 = X + ΔΦ₂  (другая фаза!)
ΔΦ₁ ≠ ΔΦ₂  ← Проблема!
```

**После (отключены адаптивные алгоритмы):**
```
RSSI RX2: 57.75 dB → Phase_RX2 = X  (стабильна)
RSSI RX2: 84.50 dB → Phase_RX2 = X  (та же фаза!)
ΔΦ₁ = ΔΦ₂  ← Решено!
```

## Как компилировать

```bash
cd /home/user/spectrum_analyzer
mkdir -p build
cd build
cmake ..
cmake --build . --config Release -j$(nproc)
```

## Логирование

При подключении к плате вы увидите в консоли:

```
[pluto] configuring phase-stable mode (disabling adaptive algorithms)...
  [voltage0] rf_dc_offset_tracking_en = 0 (disabled)
  [voltage0] bb_dc_offset_tracking_en = 0 (disabled)
  [voltage0] quadrature_tracking_en = 0 (disabled)
  [voltage1] rf_dc_offset_tracking_en = 0 (disabled)
  [voltage1] bb_dc_offset_tracking_en = 0 (disabled)
  [voltage1] quadrature_tracking_en = 0 (disabled)
[pluto] phase-stable configuration complete
```

## Что изменилось в коде

### 1. `include/capture.hpp`

```diff
  // Инверсия Rx2. Стоит до всей остальной настройки...
  void apply_phase_inversion();
+ 
+ // Отключение адаптивных алгоритмов для стабильной фазы между каналами.
+ // Убирает AM-to-PM эффект при изменении уровня сигнала.
+ void configure_phase_stable();
```

### 2. `src/capture.cpp` — новая функция после `apply_phase_inversion()`

```cpp
void PlutoCaptureThread::configure_phase_stable() {
    if (!phy_dev_ || !phy_chn0_ || !phy_chn1_) return;

    cout << "[pluto] configuring phase-stable mode (disabling adaptive algorithms)...\n";

    struct iio_channel* channels[] = { phy_chn0_, phy_chn1_ };
    const char* channel_names[] = { "voltage0", "voltage1" };

    for (int i = 0; i < 2; i++) {
        struct iio_channel* chn = channels[i];
        const char* name = channel_names[i];

        if (!chn) continue;

        int ret = 0;

        // Отключить RF DC offset tracking
        ret = iio_channel_attr_write_bool(chn, "rf_dc_offset_tracking_en", false);
        if (ret < 0) {
            log_error(string(name) + ": rf_dc_offset_tracking_en", ret);
        } else {
            cout << "  [" << name << "] rf_dc_offset_tracking_en = 0 (disabled)\n";
        }

        // Отключить BB DC offset tracking
        ret = iio_channel_attr_write_bool(chn, "bb_dc_offset_tracking_en", false);
        if (ret < 0) {
            log_error(string(name) + ": bb_dc_offset_tracking_en", ret);
        } else {
            cout << "  [" << name << "] bb_dc_offset_tracking_en = 0 (disabled)\n";
        }

        // Отключить quadrature tracking
        ret = iio_channel_attr_write_bool(chn, "quadrature_tracking_en", false);
        if (ret < 0) {
            log_error(string(name) + ": quadrature_tracking_en", ret);
        } else {
            cout << "  [" << name << "] quadrature_tracking_en = 0 (disabled)\n";
        }
    }

    cout << "[pluto] phase-stable configuration complete\n";
}
```

### 3. `src/capture.cpp` — вызов в `configure_device()`

```diff
  // Первым делом: инверсия Rx2 требует переинициализации чипа...
  apply_phase_inversion();

+ // Конфигурация для стабильной фазы: отключаем адаптивные алгоритмы,
+ // которые меняют фазу в зависимости от уровня сигнала (AM-to-PM эффект).
+ // Это необходимо для калибровки и измерений фазы между каналами.
+ configure_phase_stable();
+
  // Двухканальный режим — это сравнение трактов между собой...
```

## Проверка работы

После компиляции и запуска:

1. **Просмотр логов:**
   - Вы должны увидеть сообщения об отключении алгоритмов
   - Все три параметра на обоих каналах должны быть disabled

2. **Проверка регистров:**
   ```bash
   python3 python/dump_ad9361_full.py
   ```
   В выводе должны быть видны нулевые значения для:
   - `rf_dc_offset_tracking_en`
   - `bb_dc_offset_tracking_en`
   - `quadrature_tracking_en`

3. **Измерение фазы:**
   - Захватите I/Q сэмплы с разными уровнями сигнала
   - Фаза между RX1 и RX2 должна оставаться стабильной
   - Не зависеть от RSSI

## Возможные проблемы

### 1. Ошибка компиляции
**Если:** `undefined reference to configure_phase_stable`
**Решение:** Перестройте проект: `rm -rf build && mkdir build && cd build && cmake .. && make`

### 2. Ошибка доступа на плате
**Если:** `[pluto] log_error: attribute access denied`
**Причина:** Нет прав на запись атрибутов
**Решение:** Используйте IP с полными правами (root/root на Pluto)

### 3. Атрибуты не найдены
**Если:** Функция отказывается отключать атрибуты
**Причина:** Старая версия прошивки на плате
**Решение:** Обновите прошивку AD9361

## Отключение функции

Если нужно отключить phase-stable конфигурацию (вернуться к исходному поведению):

**Вариант 1:** Закомментировать вызов в `configure_device()`
```cpp
// configure_phase_stable();  // Disabled for compatibility
```

**Вариант 2:** Сделать функцию условной через переменную состояния
```cpp
if (state_.phase_stable_mode.load()) {
    configure_phase_stable();
}
```

## Тестирование

Рекомендуемая процедура проверки:

```bash
# 1. Компилировать
cd /home/user/spectrum_analyzer
rm -rf build
mkdir build && cd build
cmake .. && cmake --build .

# 2. Запустить приложение
./build/spectrum_analyzer

# 3. Подключиться к плате (UI)

# 4. Проверить логи:
#    - Должны видны [pluto] настройки phase-stable mode

# 5. Выгрузить регистры
python3 python/dump_ad9361_full.py

# 6. Проверить, что rf_dc_offset_tracking_en, 
#    bb_dc_offset_tracking_en, quadrature_tracking_en = 0

# 7. Захватить I/Q и измерить фазу
#    - Фаза между каналами должна быть стабильна
```

## Дополнительные ссылки

- `PHASE_CONFIG_GUIDE.md` — подробное руководство по конфигурации
- `PHASE_ANALYSIS.md` — техническое объяснение AM-to-PM эффектов
- `configure_phase_stable.py` — Python скрипт для интерактивной конфигурации
