# Merge: третий → second → main

Дата: 2026-09-25  
Репозиторий: [V1DeeK/AFARCalibration](https://github.com/V1DeeK/AFARCalibration)  
PR: [#2](https://github.com/V1DeeK/AFARCalibration/pull/2) (`second` → `main`)

## Ветки

| Ветка | Роль |
|-------|------|
| `main` | целевая |
| `second` | рабочая база, в неё вливали `третий` |
| `третий` | источник уникальных изменений |

## Стратегия

1. **третий → second** — сторона `ours` = `second`, из `третий` взяты только уникальные правки.
2. **second → main** — через PR #2.

## Что сохранено из `третий`

- `test_ui_interactions`
- правки CMake
- отчёт `tz-status`

После merge починены конфликты/регрессии: SOLT, zoom, `required_model`.

## Проверка

- Локально: **ctest 15/15**.
- CI на GitHub не настроено — merge по статусу **MERGEABLE** и локальным тестам.
