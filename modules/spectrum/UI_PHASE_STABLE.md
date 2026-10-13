# UI: Настройка Phase Stable Mode

## Где находится галочка в интерфейсе

### Расположение: Device Settings → "Phase stable mode"

**Путь в приложении:**
```
Main Window
└── Device Settings (левая колонна)
    └── Phase stable mode ☑️  ← вот здесь
```

### Визуальное расположение:

```
┌─────────────────────────────────────────┐
│ DEVICE SETTINGS                         │
├─────────────────────────────────────────┤
│                                         │
│ [Input settings]                        │
│ Center Frequency:  [5.8 GHz        ]   │
│ Sample Rate:       [20.0 MSps      ]   │
│ Bandwidth:         [20.0 MHz       ]   │
│                                         │
│ [Channel / RF]                          │
│ Input Port: [0: A_BALANCED      ▼]     │
│ Mode: [Channel_1                 ▼]   │
│                                         │
│ [Rx Calibration & Phase Control]       │
│ ☑ Invert Rx2 phase                     │
│                                         │
│ ☑ Phase stable mode  ← НОВАЯ ГАЛОЧКА  │
│                                         │
│ ☑ Apply Rx2 calibration                │
│                                         │
│ [Gain Control]                          │
│ Gain Mode:    [Manual             ▼]   │
│ Ch1 Gain:     [30  dB             ]     │
│ ...                                     │
└─────────────────────────────────────────┘
```

## Что это делает

### Когда включена (✓):
```
☑ Phase stable mode
```

- При подключении к плате автоматически отключаются адаптивные алгоритмы:
  - `rf_dc_offset_tracking_en = 0`
  - `bb_dc_offset_tracking_en = 0`
  - `quadrature_tracking_en = 0`

- **Результат:** Фаза между RX1 и RX2 остается **стабильной**, независимо от уровня входного сигнала (RSSI)

- **Когда использовать:** 
  - ✓ Калибровка приемника
  - ✓ Измерение фазы между каналами
  - ✓ Научные/точные измерения

### Когда отключена (☐):
```
☐ Phase stable mode
```

- Адаптивные алгоритмы **остаются включенными** (оригинальное поведение AD9361)

- **Результат:** Максимальная чувствительность, но фаза зависит от RSSI (AM-to-PM эффект)

- **Когда использовать:**
  - ✓ Нормальные приемные операции
  - ✓ Максимум качества сигнала
  - ✓ Когда точная фаза не критична

## Связь с другими настройками

```
Device Settings
│
├─ Invert Rx2 phase (инверсия 180°)
│  └─ Убирает штатный сдвиг фазы чипа
│
├─ Phase stable mode ← новая настройка
│  └─ Убирает AM-to-PM эффект от адаптивных алгоритмов
│
└─ Apply Rx2 calibration (таблица калибровки)
   └─ Убирает остаточный перекос после первых двух
```

**Обе первые настройки работают вместе:**
- `Invert Rx2 phase`: Убирает знаковый сдвиг (~180°)
- `Phase stable mode`: Убирает зависимость фазы от уровня сигнала
- `Apply Rx2 calibration`: Убирает оставшийся перекос, измеренный генератором

## Влияние на логи

При подключении с включенной галочкой:

```
[pluto] attempting connection...
[pluto] connection succeeded on ip:fcoms.local

[pluto] configuring phase-stable mode (disabling adaptive algorithms)...
  [voltage0] rf_dc_offset_tracking_en = 0 (disabled)      ← видны эти строки
  [voltage0] bb_dc_offset_tracking_en = 0 (disabled)
  [voltage0] quadrature_tracking_en = 0 (disabled)
  [voltage1] rf_dc_offset_tracking_en = 0 (disabled)
  [voltage1] bb_dc_offset_tracking_en = 0 (disabled)
  [voltage1] quadrature_tracking_en = 0 (disabled)
[pluto] phase-stable configuration complete

[pluto] successfully connected and streaming
```

При подключении с отключенной галочкой:

```
[pluto] attempting connection...
[pluto] connection succeeded on ip:fcoms.local
[pluto] successfully connected and streaming
← фазы настройки видно не будет
```

## Всплывающая подсказка (Tooltip)

Если навести мышь на галочку, появляется подсказка:

```
Disable AD9361 adaptive algorithms (RF/BB DC offset tracking,
quadrature tracking) for phase-stable measurements between RX1/RX2.
Removes AM-to-PM effect when signal level changes (RSSI variation).
Required for accurate phase calibration; applied at reconnect.
```

## Как использовать

### Сценарий 1: Калибровка фазы

```
1. Установить галочку ☑ Phase stable mode
2. Подключиться к плате ("Connect" кнопка)
3. Ждать сообщения "[pluto] successfully connected and streaming"
4. Захватить I/Q данные (кнопка "Save IQ")
5. Фаза между RX1 и RX2 будет стабильной
```

### Сценарий 2: Нормальная работа

```
1. Оставить галочку отключенной ☐ Phase stable mode (по умолчанию)
2. Подключиться к плате
3. Использовать приложение нормально
4. Максимум чувствительности и качества
```

### Сценарий 3: Срочная переключение

```
1. Если уже подключены к плате с одной настройкой
2. Переключить галочку (вкл/выкл)
3. Нажать "Disconnect" кнопку
4. Нажать "Connect" кнопку
5. Новая конфигурация применится
```

## Эффект на производительность

- **Чтение:** Нет влияния (алгоритмы работают на плате)
- **Захват:** Нет влияния
- **Обработка:** Нет влияния
- **Качество сигнала:** Может слегка измениться из-за отключения адаптивных компенсаций

## Совместимость

| Плата | Поддержка |
|-------|-----------|
| PlutoSDR | ✓ Да |
| FMCOMMS5 | ✓ Да |
| FMCOMMS4 | ✓ Да |
| Любая с AD9361 | ✓ Да (если прошивка поддерживает эти атрибуты) |

## Возможные проблемы

### Проблема: Галочка не работает / не применяется

**Решение:**
1. Убедитесь, что вы подключены к плате
2. Переключите галочку
3. Нажмите кнопку "Disconnect"
4. Нажмите кнопку "Connect"
5. Проверьте логи для сообщений об ошибках

### Проблема: Ошибка в логе "attribute access denied"

**Причина:** Нет прав на запись атрибутов
**Решение:** 
- Используйте учетные данные root/root на PlutoSDR
- Проверьте версию прошивки (может быть слишком старая)

### Проблема: Настройки сбрасываются при перезагрузке платы

**Это нормально!** Настройка применяется только на время сеанса.
При каждом подключении функция `configure_phase_stable()` вызывается заново.

## Файлы, которые изменились

1. **include/app_state.hpp** (строка 113)
   - Добавлена переменная `phase_stable_mode`

2. **src/display.cpp** (строка 600)
   - Добавлен checkbox "Phase stable mode"
   - Добавлена всплывающая подсказка

3. **src/capture.cpp** (строка 221)
   - Добавлена условная проверка `if (state_.phase_stable_mode.load())`
   - Вызов `configure_phase_stable()` только если включено

## Дополнительная информация

- **PHASE_STABLE_CONFIG.md** — полная техническая документация
- **PHASE_CONFIG_GUIDE.md** — руководство по конфигурации
- **PHASE_ANALYSIS.md** — объяснение AM-to-PM эффектов
- **python/configure_phase_stable.py** — Python скрипт для конфигурации
