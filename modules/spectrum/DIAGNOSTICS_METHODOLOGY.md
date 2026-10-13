# Диагностика изменения Δφ(RSSI): Методология

## Ваш анализ (правильный!)

| Параметр | snapshot1 | snapshot2 |
|----------|-----------|-----------|
| **RSSI** | 57.75 dB | 84.50 dB (слабее на 26.75 dB) |
| Gain control | manual | manual |
| Видимое усиление | 20 dB | 20 dB |
| RX LO | 5806 MHz | 5806 MHz |
| Fs | 16 MHz | 16 MHz |
| Полоса | 16 MHz | 16 MHz |
| Quadrature tracking | **ВКЛ** | **ВКЛ** |
| RF/BB DC tracking | **ВКЛ** | **ВКЛ** |
| Температура | 29.8°C | 31.6°C (+1.8°C) |

**Вывод:** Настройки одинаковые → причина в **уровне сигнала и/или адаптивных алгоритмах**, работающих во время приема.

---

## Четыре гипотезы

### 1️⃣ Переключение LNA (~180° скачок фазы)

**Признаки:**
- Резкое изменение фазы (скачок)
- Коррелирует с большим изменением RSSI

**Как проверить:**
```bash
python3 python/diagnose_phase_shift.py --uri "ip:fcoms.local"
# Ищем в выводе: gaininfo_rx1, gaininfo_rx2
# Проверить: изменился ли lna_index?
```

**Если true:**
- LNA переключился (обычно индекс 0→1 или 1→2)
- ADI подтверждает: это причина фазовых скачков
- Регистр 0x022[D6]: "Invert Bypassed LNA Polarity"

**Решение:** Закрепить LNA на одном пути или допустить скачок как нормальный.

---

### 2️⃣ Клиппирование (AM-to-PM в компрессии)

**Признаки:**
- На сильном сигнале (RSSI 57.75) → клиппирование
- На слабом сигнале (RSSI 84.50) → нет
- Фаза коррелирует с наличием клиппирования

**Как проверить:**
```bash
python3 python/capture_iq_analysis.py \
    --uri "ip:fcoms.local" \
    --freq 5.806e9 \
    --gain 20 \
    --samples 16384 \
    --output results_20db.json

# Ищем в выводе:
# "⚠️  CLIPPING DETECTED!"
# Max I/Q value близко к (2^15 - 1) = 32767
```

**Если true:**
- На одном уровне сигнала клиппирование, на другом нет
- Это вызовет разные фазовые характеристики (AM-to-PM)
- Усилители имеют разные пороги на RX1 vs RX2

**Решение:** Снизить gain на 5-10 дB или использовать аттенюатор.

---

### 3️⃣ Разные компрессии RX1 vs RX2 (нелинейность без клиппирования)

**Признаки:**
- Клиппирования нет, но фаза меняется
- Зависимость фазы от уровня сигнала (Δφ ≠ const)
- Одна веточка может быть на краю компрессии

**Как проверить:**
```bash
# Sweep мощности при фиксированном gain = 0 dB (безопаснейший режим)
python3 python/capture_iq_analysis.py \
    --uri "ip:fcoms.local" \
    --gain 0 \
    --sweep \
    --output sweep_0db.json

# Сравнить результаты sweep:
# Если Δφ flat → компрессия была причиной
# Если Δφ still varies → что-то еще
```

**Если true:**
- Фаза зависит от амплитуды из-за нелинейности
- Один канал может быть чувствительнее
- Усилители RX1 и RX2 немного отличаются

**Решение:** Использовать меньшее усиление или дополнительную калибровку Δφ(level).

---

### 4️⃣ Tracking-контуры (rf_dc_offset_tracking, quadrature_tracking)

**Признаки:**
- Включены: `quadrature_tracking_en = 1`, `rf_dc_offset_tracking_en = 1`
- Фаза стабилизируется, если их отключить
- Зависимость от уровня сигнала и качества сигнала

**Как проверить:**
```bash
# Текущее состояние (с tracking ВКЛ):
python3 python/diagnose_phase_shift.py --uri "ip:fcoms.local"

# Отключить через UI или Python:
# state_.phase_stable_mode = true  (в приложении)

# Переподключиться
# Повторить измерение
# Сравнить Δφ

# Если phase stabilizes → tracking был виноват
# После теста: включить обратно!
```

**Если true:**
- Quadrature tracking непрерывно оценивает I/Q-дисбаланс
- На разных уровнях сигнала оценка дает разные компенсации
- Это **предусмотренное поведение AD9361**

**Решение:** Отключить tracking (если точная фаза более важна, чем качество) или калибровать зависимость Δφ(RSSI).

---

## 🔴 КРИТИЧЕСКИЙ МОМЕНТ: Тон на Найквисте!

**Текущая конфигурация:**
```
RX_LO = 5806 MHz
Fs = 16 MHz  (Nyquist = 8 MHz)
Test tone ≈ 5806 + 7.999389 MHz = 5813.999389 MHz

После down-conversion:
IF tone ≈ 7.999389 MHz (почти на Nyquist!)
```

**Проблема:**
- Частота ровно на Найквисте → максимальная чувствительность к смещению в одном отсчете
- Если RX1 и RX2 захватывают на разные смещения:
  ```
  Δn = ±1 sample  →  Δφ ≈ 180°
  ```
- Это может быть **ошибка прибора**, а не физическая фаза!

**Решение — ИЗМЕНИТЬ ТЕСТОВЫЙ ТОН:**
```
Текущий (ПЛОХО):  Fsignal = RX_LO + 8 MHz = 5814 MHz (почти Nyquist)
Рекомендуемый:    Fsignal = RX_LO + 1 MHz = 5807 MHz (середина полосы)
Альтернатива:     Fsignal = RX_LO + 2 MHz = 5808 MHz (если 1 MHz занята)
```

**Почему это имеет значение:**
- На 1 MHz от LO: фаза меньше зависит от джиттера захвата
- На полусредней полосе: лучше амплитудный ответ фильтра
- Подальше от Nyquist: меньше цифровых артефактов

---

## 📋 Полная диагностическая процедура

### Этап 1: Сбор информации о трактах

```bash
cd /home/user/spectrum_analyzer

# Снять подробную информацию о трактах
python3 python/diagnose_phase_shift.py --uri "ip:fcoms.local" \
    --output diagnostic_report.txt

# Проверить:
# - Есть ли gaininfo для RX1 и RX2?
# - Виден ли lna_index для каждого?
# - Какие tracking-алгоритмы включены?
```

**Ожидаемый вывод:**
```
DETAILED GAIN INFORMATION (RX1 & RX2)
⚠️  Note: Check for lna_index changes between snapshots!

rx1:
  gaininfo_rx = "LNA_index=1 ..."  ← ИЩЕМ ЭТО
  ...
```

---

### Этап 2: Проверка клиппирования

```bash
# Захватить при ТЕКУЩЕМ усилении (20 дB)
python3 python/capture_iq_analysis.py \
    --uri "ip:fcoms.local" \
    --freq 5.807e9 \
    --gain 20 \
    --samples 16384 \
    --output iq_20db.json

# Результат:
# ✓ No clipping detected        → нет компрессии
# ⚠️  CLIPPING DETECTED! (X.XX%)  → есть перегруз
```

---

### Этап 3: Sweep мощности при безопасном gain

```bash
# Sweep от 0 до 30 dB, шаг 5 dB
python3 python/capture_iq_analysis.py \
    --uri "ip:fcoms.local" \
    --freq 5.807e9 \
    --sweep \
    --output sweep_result.json

# Анализ результата (в файле sweep_result.json):
# Если Δφ примерно constant ± 2° → нет компрессии
# Если Δφ линейно растет/падает → компрессия или tracking
```

---

### Этап 4: Тест отключения tracking

```bash
# 1. В приложении включить "Phase stable mode" галочку
#    (это отключит tracking-алгоритмы)

# 2. Переподключиться к плате

# 3. Повторить capture:
python3 python/capture_iq_analysis.py \
    --uri "ip:fcoms.local" \
    --freq 5.807e9 \
    --gain 20 \
    --samples 16384 \
    --output iq_20db_nostable.json

# 4. Сравнить Δφ с предыдущим результатом
# Если Δφ ≈ const → tracking было причиной
# Если Δφ меняется → что-то еще (компрессия или LNA)

# 5. ВАЖНО: Вернуть tracking обратно!
```

---

### Этап 5: Анализ результатов

**Создать сводную таблицу:**

```
Условие                    Δφ (градусы)    Клиппирование    Вывод
─────────────────────────────────────────────────────────────────
Gain 20 dB, tracking ВКЛ    X₁              да/нет          исходное
Gain 0 dB, sweep            ~flat           нет             ?
Gain 20 dB, tracking ВЫК    X₂              да/нет          ?
Frequency 5807 MHz          Y               да/нет          ?
─────────────────────────────────────────────────────────────────
```

**Логика вывода:**
```
Если X₁ ≠ X₂:
  → tracking-контуры влияют на Δφ
  
Если sweep flat (gain 0):
  → нет компрессии/LNA-switching
  
Если sweep not flat:
  → компрессия или LNA-switching
  
Если Y ≠ X (на новой частоте):
  → старая частота была на Nyquist, это было ошибкой
```

---

## 📊 JSON результаты

Оба скрипта выдают JSON для анализа:

**diagnose_phase_shift.py:**
```json
{
  "device": "ad9361-phy",
  "gaininfo_rx1": "...",
  "gaininfo_rx2": "...",
  "tracking_status": {
    "rf_dc_offset_tracking_en": "1",
    "quadrature_tracking_en": "1"
  }
}
```

**capture_iq_analysis.py --sweep:**
```json
{
  "results": [
    {"label": "Gain=0dB", "phase_deg": 45.23, "clipping_rx1": false, "clipping_rx2": false},
    {"label": "Gain=5dB", "phase_deg": 45.31, "clipping_rx1": false, "clipping_rx2": false},
    ...
    {"label": "Gain=30dB", "phase_deg": 67.89, "clipping_rx1": true, "clipping_rx2": false}
  ]
}
```

---

## ✅ Чек-лист

- [ ] Запустить `diagnose_phase_shift.py` и проверить gaininfo
- [ ] Запустить `capture_iq_analysis.py` при Gain=20dB
- [ ] Запустить `capture_iq_analysis.py --sweep`
- [ ] Включить "Phase stable mode" в UI
- [ ] Переподключиться и повторить `capture_iq_analysis.py`
- [ ] Создать таблицу результатов
- [ ] Сравнить Δφ при разных условиях
- [ ] Вернуть tracking обратно

---

## 🎯 Ожидаемые результаты

### Сценарий 1: LNA-switching виновата
```
gaininfo показывает: lna_index меняется между измерениями
Вывод: Δφ ≈ 180° на каком-то пороге уровня
```

### Сценарий 2: Компрессия виновата
```
sweep показывает: Δφ растет с gain
capture_iq_analysis показывает: клиппирование при высоком gain
Вывод: снизить gain или использовать аттенюатор
```

### Сценарий 3: Tracking виноват
```
С tracking: Δφ = 45°
Без tracking: Δφ = 35°
Вывод: tracking добавляет 10° сигнал-зависимого сдвига
```

### Сценарий 4: Nyquist артефакт
```
На 5807 MHz: Δφ стабильна
На 5814 MHz: Δφ скачет
Вывод: тон был слишком близко к Nyquist
```

---

## 📝 Результаты для отчета

Сохраните результаты в один файл:
```bash
cat > diagnostic_results.md << 'EOF'
# Диагностика Δφ(RSSI)

## Результаты

### Этап 1: Gaininfo
[результат diagnose_phase_shift.py]

### Этап 2: Клиппирование при Gain=20dB
[результат capture_iq_analysis.py]

### Этап 3: Sweep Gain 0-30 dB
[результат sweep, таблица Δφ vs Gain]

### Этап 4: Без tracking
[результат capture_iq_analysis.py с отключенным tracking]

### Этап 5: На новой частоте 5807 MHz
[результат capture_iq_analysis.py с новой частотой]

## Выводы
[ваше резюме]
EOF
```
