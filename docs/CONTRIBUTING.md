# Совместная разработка

Репозиторий: [V1DeeK/AFARCalibration](https://github.com/V1DeeK/AFARCalibration).  
Участники: **V1DeeK**, **Layt-xxl**.

## Правила веток

- `main` — стабильная сборка; пушить напрямую только мелкие правки документации по согласованию.
- Фичи и багфиксы — в отдельных ветках: `feat/…`, `fix/…` от актуального `main`.
- Перед слиянием — Pull Request в `main`, хотя бы один просмотр другого разработчика.

## Локальный цикл

```bat
git clone https://github.com/V1DeeK/AFARCalibration.git
cd AFARCalibration
git checkout -b feat/короткое-имя
rem … правки …
git add <файлы>
git commit -m "feat(scope): описание на русском"
git push -u origin HEAD
gh pr create
```

Сборка и `ctest` — см. корневой `README.md`. Каталог `build/` в git не коммитится.

## Что не класть в git

- `build/`, `*.exe`, `*.dll`, пароли, боевые IP стенда
- `.cursor/workspace/` (локальное состояние оркестрации)
