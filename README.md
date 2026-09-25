# AFAR RX Calibration Studio

Настольная программа **этапа 1** по ТЗ `AFAR-RX-CAL-SW-TZ-01`: характеризация **16 приёмных каналов** активной фазированной антенной решётки (АФАР).

Программа задаёт состояние изделия и снимает комплексную S21 через ПО **S2VNA**. Серийный цикл АФАР и LUT сохранён, а отдельный одиночный свип поддерживает двухпортовые **ПЛАНАР C1220/C2220**. Нормативные требования — в [`afar_stage1_software_tz.pdf`](afar_stage1_software_tz.pdf); этот файл — вход в репозиторий, а не пересказ ТЗ.

CMake-имя проекта: `AfarRxCalibrationStudio`. Целевая ОС: Windows 10/11 x64.

## Границы v1.0

**Входит:** одиночное измерение фильтра на C1220/C2220, а также C2220 и контроллер изделия для полного цикла серии этапа 1, QC, восстановления, прямой и обратной LUT, каталога серии и протокола.

**Не входит** (этапы 2/3, в v1.0 не реализуются): сканер ближнего поля, кодирование Хадамарда, диаграммы направленности лучей, моноимпульс. Для них в контрактах зарезервированы имена схем и точки расширения — без кода.

Измерения на стенде — ПЛАНАР C1220/C2220 через S2VNA Socket. Другие анализаторы и иная SCPI-сессия в v1 не поддерживаются.

## Стек

| Компонент | Версия / роль |
|-----------|----------------|
| Язык | C++20 (`CMAKE_CXX_STANDARD 20`, расширения выключены) |
| Сборка | CMake ≥ 3.22, CTest |
| Компилятор | MinGW/UCRT64 (`g++`), генератор `MinGW Makefiles` |
| GUI | Qt 6 |
| Тесты | Catch2 v3.7.1 (FetchContent при конфигурации) |
| Сырой S21 / LUT | собственные контейнеры **AFARH5** и **AFARPQ** (имена файлов `raw-s21.h5`, `*.parquet` по каталогу серии) — **без** системных libhdf5 и Apache Arrow |

Рабочий транспорт S2VNA — **TCP Socket**. Старый последовательный `ScpiComTransport` оставлен только для совместимости кода и скрыт из GUI: он не является COM/DCOM Automation S2VNA. Порт и хост задаются пользователем; стандартный Socket-порт S2VNA — `5025`.

Боевые IP стенда в git не кладутся.

Реальный контроллер изделия до передачи протокола (т. 14 ТЗ) — **заглушка** `StubDutController`: COM/TCP изделия не открывает. Рабочие прогоны и GUI по умолчанию идут через `VnaSimulator` + `DutSimulator`.

## Зависимости для конфигурации

1. **CMake 3.22+** и **Git** (первый `cmake -S` качает Catch2 и nlohmann_json).
2. **MinGW/UCRT64** — в `PATH` сессии: `g++.exe`, `mingw32-make.exe`.
3. **Qt 6** — префикс через `CMAKE_PREFIX_PATH`. `find_package(Qt6 … REQUIRED)` только для exe `AfarRxCalibrationStudio`; библиотеки `afar_*` и их тесты Qt не линкуют (кроме самого GUI).

Живой C1220/C2220 для сборки и обычного `ctest` не нужен: приёмка AT-01…AT-11 на имитаторах и TCP-stub. Прибор нужен для измерений на стенде и для аппаратной части AT-01.

## Сборка и тесты

Каталог `build/` — артефакт конфигурации (см. `.gitignore`). Генератор — **MinGW Makefiles**.

```bat
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=C:/msys64/ucrt64/bin/g++.exe -DCMAKE_PREFIX_PATH=C:/msys64/ucrt64
cmake --build build
ctest --test-dir build --output-on-failure
```

Если MinGW `bin` уже в `PATH` и задан `CMAKE_PREFIX_PATH`:

```bat
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

На MinGW 13 флаг `-O3` в Release заменяется на `-O2` (ICE GCC). Предупреждения — как ошибки (`-Wall -Wextra -Wpedantic -Werror`).

### Список CTest (14)

| Имя | Назначение |
|-----|------------|
| `smoke` | дымовой Catch2 |
| `run_config` | AT-02: разбор `run-config` / CSV аттенюатора |
| `lut_math` | AT-09: нормировка, фаза, прямая/обратная LUT |
| `quality_gates` | AT-10: флаги QC |
| `simulators` | AT-01 на имитаторах VNA/DUT |
| `hdf5_store` | AFARH5 / `raw-s21.h5` |
| `probe_run` | AT-03: короткий цикл на имитаторах |
| `pause_resume` | AT-05: пауза / продолжение |
| `crash_recovery` | AT-06: восстановление после сбоя |
| `faults` | AT-07 / AT-08: тайм-аут VNA, отказ DUT |
| `export_manifest` | AT-11: LUT/PDF/манифест SHA-256 (тот же путь — `finalizeExports` в Finalizing при Complete) |
| `c2220_driver` | AT-01 драйвер: Socket + TCP-stub SCPI |
| `full_sim_run` | AT-04: компактная сетка в CI; полный объём — см. ниже |
| `lut_perf` | замер LUT (probe в CI; полный объём — см. ниже) |

Выборочно: `ctest --test-dir build -R "smoke|simulators" --output-on-failure`.

### Полная серия AT-04 (`AFAR_RUN_AT04`)

По умолчанию `full_sim_run` и `lut_perf` гоняют **компактную** сетку (быстрый CI). Полные 65 536 состояний / полный объём LUT — только явно:

```bat
set AFAR_RUN_AT04=1
build\tests\test_full_sim_run.exe "[full]"
build\tests\test_lut_perf.exe "[full]"
```

Без `AFAR_RUN_AT04=1` кейсы с тегом `[full]` пропускаются (`SKIP`).

## Запуск GUI

После сборки:

```bat
build\src\AfarRxCalibrationStudio.exe
```

Нужны DLL Qt в `PATH` (или рядом с exe). По умолчанию железо **не** трогается: режим **Имитатор**. Для живого C1220/C2220 — см. [`docs/S2VNA-setup.md`](docs/S2VNA-setup.md): запустить S2VNA, включить Socket 5025, выбрать **S2VNA Socket** и **«Проверить связь»**. На экране **«4 S-параметры / фильтр»** доступны S11/S21/S12/S22 и единицы Гц/кГц/МГц/ГГц. Кнопка измерения строит графики с координатной сеткой; отдельная кнопка сохраняет последнее измерение как `s_parameter,frequency_hz,real,imag,magnitude_db,phase_deg`.

Этап **«2 Калибровка VNA»** содержит пошаговую полную двухпортовую SOLT: соединение, OPEN/SHORT/LOAD обоих портов, THRU и применение. Перед началом в S2VNA выбирается фактический комплект калибровочных мер.

### Быстрый прогон на имитаторах

1. Слева этап **«1 Подключения»** или синяя кнопка **«Настроить серию АФАР»**.
2. На каждом шаге мастера — галочка **«Подтверждаю»** → **«Далее»**; финиш — **«Готово — подготовить серию»** (статус READY).
3. Частоты/точки на этапе **«4 Перебор кодов»** попадают в серию при следующем мастере.
4. Зелёная **«Старт серии АФАР»**; на паузе она становится **«Продолжить серию»**. Красный **«Стоп»** с подтверждением (FR-11).

Одиночные S11/S21/S12/S22 не используют мастер, контроллер изделия и кнопку старта серии: для них достаточно зелёного состояния **«S-параметры: ГОТОВО»** и кнопки **«Измерить выбранный S-параметр»** на этапе 4. График масштабируется колесом мыши, перемещается перетаскиванием левой кнопкой и сбрасывается двойным щелчком или кнопкой **«Показать весь диапазон»**.
5. Фиолетовая **«Продолжить незакрытую серию»** — FR-12, если в каталоге данных есть серия без `report.pdf`.
6. Кнопка **«Тема»** в верхней полосе — светлая/тёмная (сохраняется в `QSettings`).

Подробности GUI UX: [`docs/reports/2026-09-23-afar-gui-ux.md`](docs/reports/2026-09-23-afar-gui-ux.md).

## CMake-цели

| Цель | Тип | Содержание |
|------|-----|------------|
| `afar_interfaces` | INTERFACE | `include/afar/` — `IVna`, `IDutController`, типы свипа |
| `afar_vna` | STATIC | Socket/COM SCPI, драйвер C1220/C2220 (`C2220Vna`) |
| `afar_core` | STATIC | `RunId` |
| `afar_data` | STATIC | run-config, CSV, каталог серии, AFARH5, JSONL |
| `afar_cal` / `afar_qc` | STATIC | LUT и QC |
| `afar_measure` | STATIC | автомат, порядок скана, оркестратор |
| `afar_sim` | STATIC | `VnaSimulator`, `DutSimulator` |
| `afar_dut` | STATIC | `StubDutController` (до т. 14) |
| `afar_report` | STATIC | AFARPQ, манифест, PDF |
| `AfarRxCalibrationStudio` | EXE | Qt Widgets GUI |

## Карта каталогов

| Путь | Назначение |
|------|------------|
| `src/app/` | `main.cpp`, точка входа GUI |
| `src/measure/` | автомат серии, оркестратор, порядок обхода |
| `src/vna/` | транспорт SCPI (Socket, COM) и драйвер C1220/C2220 |
| `src/dut/` | заглушка контроллера изделия до протокола т. 14 |
| `src/data/` | конфиг, CSV, каталог серии, AFARH5, журнал |
| `src/cal/` | нормировка, фазовая математика, LUT |
| `src/qc/` | флаги качества |
| `src/report/` | AFARPQ LUT, PDF, манифест SHA-256 |
| `src/sim/` | имитаторы C2220 и контроллера |
| `src/ui/` | Qt GUI |
| `src/core/` | идентификатор прогона |
| `include/afar/` | публичные заголовки интерфейсов |
| `tests/` | Catch2 + CTest |
| `cmake/` | Catch2, JSON, предупреждения |
| `schemas/` | JSON Schema конфигурации серии |
| `examples/` | примеры конфигов (не боевой стенд) |
| `docs/contracts/` | контракты SCPI, интерфейсов, форматов |
| `docs/plans/` | планы работ |
| `docs/reports/` | отчёты по волнам / оркестрации |
| `Planar_documentation/` | локальные руководства C2220 / S2VNA |
| `afar_stage1_software_tz.pdf` | ТЗ этапа 1 |

## Форматы данных (канон этапа 1)

- **`raw-s21.h5`** — бинарный контейнер с magic **AFARH5** (`afar::RawS21Store`), без libhdf5. Семантика осей/слотов — [`docs/contracts/data-formats.md`](docs/contracts/data-formats.md) §14.
- **`direct-lut.parquet` / `inverse-lut.parquet`** — columnar TSV UTF-8 с magic **AFARPQ** (`afar::report::ParquetExport`), без Apache Arrow. §15 того же контракта.

Имена файлов в каталоге серии сохранены по ТЗ; системные HDF5/Arrow в сборку не входят.

## Документация

- ТЗ: [`afar_stage1_software_tz.pdf`](afar_stage1_software_tz.pdf)
- SCPI C1220/C2220 / S2VNA: [`docs/contracts/vna-c2220-scpi.md`](docs/contracts/vna-c2220-scpi.md)
- C++-интерфейсы и автомат: [`docs/contracts/cpp-interfaces.md`](docs/contracts/cpp-interfaces.md)
- Каталог серии и форматы: [`docs/contracts/data-formats.md`](docs/contracts/data-formats.md)
- Итог волн 0–10: [`docs/reports/2026-09-22-afar-stage1-wave0-10.md`](docs/reports/2026-09-22-afar-stage1-wave0-10.md)
- Сверка ТЗ / GAP: [`docs/reports/2026-09-24-afar-tz-gap.md`](docs/reports/2026-09-24-afar-tz-gap.md)
- Документация прибора: [`Planar_documentation/`](Planar_documentation/)

Синтаксис SCPI сверяется с **установленной** на стенде версией S2VNA. Программа **не включает** прямой доступ к приёмникам (`SYSTem:RECeiver:DIRect:ACCess`); если режим уже ON и нет явного профиля стенда, серия не стартует.
