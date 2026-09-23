# Установка S2VNA и подключение к программе

## Что нельзя поставить автоматически

**S2VNA** (ПО ПЛАНАР для C2220) **нет в winget/Chocolatey** и не лежит в репозитории: это ПО производителя с диска прибора / личного кабинета / поставки. Скачать и поставить его должен инженер стенда.

Сайт: [planarchel.ru](https://planarchel.ru/) · поддержка: `welcome@planarchel.ru`, 8 (800) 222-12-11.  
Документация в репозитории: `Planar_documentation/`, контракт SCPI: `docs/contracts/vna-c2220-scpi.md`.

## Порядок на стенде

1. Установить **S2VNA** с носителя ПЛАНАР.
2. Подключить **C2220**, запустить S2VNA, дождаться связи с прибором.
3. В S2VNA: **System → Misc Setup → Network Remote Control** — включить **Socket Server**, порт обычно **5025**.
4. В AFAR RX Calibration Studio (верхняя полоса):
   - режим **S2VNA Socket**;
   - Host `127.0.0.1` (если S2VNA на этом ПК) или IP ПК со S2VNA;
   - Port `5025`;
   - кнопка **«Проверить связь»** → должен прийти `*IDN?` с `C2220`.
5. Мастер запуска → Старт.

Альтернатива: **S2VNA COM** + имя порта (`COM3` …).

## Что уже в программе

| Режим | Когда |
|-------|--------|
| **Имитатор** | Без прибора (разработка, CI) |
| **S2VNA Socket** | Живой C2220 через TCP |
| **S2VNA COM** | Живой C2220 через COM |

Контроллер изделия пока **DutSimulator / Stub** — до протокола т. 14 ТЗ.

## Уже установлено для сборки (этот ПК)

- MinGW 13 + Qt 6.8.3 `mingw_64`
- CMake, Catch2 (FetchContent)

Если Qt не в `PATH` при запуске exe:

```bat
set PATH=C:\Qt\6.8.3\mingw_64\bin;C:\Qt\Tools\mingw1310_64\bin;%PATH%
build\src\AfarRxCalibrationStudio.exe
```
