---
name: build-and-test
description: Точные команды сборки и прогона тестов AFAR RX Calibration Studio — CMake, Ninja, Qt 6 MinGW/UCRT64, Catch2, CTest, а также разбор типовых ошибок этого тулчейна. Применяется, когда нужно собрать проект, запустить тесты или понять ошибку сборки.
---

# Сборка и тесты

Тулчейн: MinGW/UCRT64 (`g++`), Qt 6, Ninja, CMake ≥ 3.22, генератор `Ninja`,
конфигурация `Release`. Каталог сборки — `build/` (единственный). Первая
конфигурация качает Catch2 v3.7.1 и nlohmann_json 3.11.3 через FetchContent —
нужен интернет и Git.

## Команды

```bat
rem Конфигурация с нуля (MSYS2 UCRT64)
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_CXX_COMPILER=C:/msys64/ucrt64/bin/c++.exe ^
  -DCMAKE_MAKE_PROGRAM=C:/msys64/ucrt64/bin/ninja.exe ^
  -DCMAKE_PREFIX_PATH=C:/msys64/ucrt64

rem Сборка и тесты
cmake --build build --parallel 12
ctest --test-dir build --output-on-failure
ctest --test-dir build -R "smoke|simulators" --output-on-failure
ctest --test-dir build -N            &rem только список тестов

rem Запуск GUI
build\src\AfarRxCalibrationStudio.exe
```

Список тестов — `build.tests` в `.claude/config.json`.

Длинные прогоны (не для обычного цикла):

```bat
set AFAR_RUN_AT04=1 & build\tests\test_full_sim_run.exe "[full]"
set AFAR_RUN_AT12=1 & build\tests\test_at12_soak.exe
```

Поставка: `tools\package-windows.ps1` (runtime ZIP, SHA-256, SBOM в `dist/`).
Проверка каталога серии: `tools\verify-series.ps1 <каталог>`.

## Linux (облачная сессия Claude, без прибора)

```bash
sudo apt-get install qt6-base-dev ninja-build cmake g++
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
QT_QPA_PLATFORM=offscreen ctest --test-dir build --output-on-failure --timeout 120
```

`-Werror` действует только на MinGW, поэтому на Linux проверяй предупреждения
глазами. Известные особенности Linux: `verify_series_tool` не регистрируется
без PowerShell; кейс `Socket abort interrupts a blocked read` в `c2220_driver`
висит (на Linux `close()` не будит заблокированный `recv`); `ui_interactions`
падает при выходе из-за статического `QApplication`.

## Типовые ошибки этого тулчейна

**Новый файл не участвует в сборке.** Его нужно добавить в список исходников
нужной библиотеки `afar_*` в `src/CMakeLists.txt` (или в `tests/CMakeLists.txt`).

**Предупреждение остановило сборку.** На MinGW `-Wall -Wextra -Wpedantic
-Werror`. Чини причину, не выключай флаг.

**Внутренняя ошибка компилятора.** GCC 13 MinGW падает на `-O3`; в проекте
`-O3` принудительно заменён на `-O2`. Не возвращай `-O3`.

**moc падает на `moc_predefs.h`.** Вложенные кавычки в `-D` ломают AUTOMOC на
MinGW, поэтому у `AfarRxCalibrationStudio` `AUTOMOC_COMPILER_PREDEFINES OFF`.

**exe не стартует, нет Qt DLL/плагинов.** Добавь `C:\msys64\ucrt64\bin` в
`PATH` или разверни `windeployqt` / `tools\package-windows.ps1`.

**Тест не виден `ctest`.** Не хватает `add_test(NAME ... COMMAND ...)` в
`tests/CMakeLists.txt`.

**Кейс помечен SKIP.** Теги `[full]` и AT-12 требуют переменных окружения —
это не падение.

## Правила

- Не трогай `build/` руками и не коммить его содержимое.
- Не меняй версию Qt, компилятор и флаги оптимизации без явной просьбы.
- Сломанная сборка — это результат работы, о котором надо сообщить сразу,
  а не пытаться прогнать тесты поверх неё.
