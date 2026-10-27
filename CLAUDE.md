# AFAR RX Calibration Studio

Программа стендовой калибровки 16 приёмных каналов АФАР (этап 1 по ТЗ
`AFAR-RX-CAL-SW-TZ-01`, `afar_stage1_software_tz.pdf`). C++20, Qt 6, CMake +
Ninja, Catch2. Целевая платформа — Windows 10/11 x64, MinGW/UCRT64. Интерфейс и
вся документация — на русском языке. Отвечай пользователю по-русски.

Решётка, которую калибрует эта программа, — приёмная АФАР изделия, для которого
пишется АРМ в репозитории `V1DeeK/RMO`. Результат программы — LUT кодов
аттенюатора/фазовращателя по каналам. Код и протокол РМО ↔ PS сюда не
переносятся.

## Раскладка

- `include/afar/` — публичные интерфейсы `IVna`, `IDutController`, типы свипа
- `src/vna` (SCPI → S2VNA), `src/dut` (заглушка контроллера изделия),
  `src/measure` (автомат и оркестратор), `src/data`, `src/cal`, `src/qc`,
  `src/report`, `src/sim`, `src/core`, `src/ui`, `src/app`
- `tests/` — Catch2 + CTest
- `docs/contracts/` — контракты: SCPI, C++-интерфейсы, форматы данных, коды
- `docs/plans/`, `docs/reports/` — планы и отчёты; рабочий план —
  `docs/plans/2026-10-03-plan-7-etapov.txt`
- `Planar_documentation/` — руководства C2220 / S2VNA (справочно)

## Сборка и тесты

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=<Qt>
cmake --build build --parallel
ctest --test-dir build --output-on-failure
ctest --test-dir build -R c2220_driver --output-on-failure   # один тест
```

Тесты: `smoke`, `run_config`, `lut_math`, `quality_gates`, `simulators`,
`hdf5_store`, `probe_run`, `pause_resume`, `crash_recovery`, `faults`,
`export_manifest`, `two_port_export`, `c2220_driver`, `full_sim_run`,
`lut_perf`, `at12_soak`, `verify_series_tool` (только при PowerShell),
`ui_interactions`, `ppkh4_frame`, `ppkh4_state`. Подробности и типовые ошибки — скилл `build-and-test`.

## Жёсткие ограничения

- ПЛАНАР C2220 управляется только так: программа → SCPI по TCP Socket →
  S2VNA → прибор. Прямой USB/Ethernet-драйвер и угадывание внутреннего
  протокола ПЛАНАРа запрещены (лицензия ПЛАНАР, п. 1.5 и 5.2). S2VNA на
  этом ПК (Windows) программа по кнопке «Вкл» может запустить сама и скрыто,
  только документированными ключами (`/SocketServer:on /SocketPort:N
  /visible:off /simulate:off`), и закрывает только свою копию. Уже
  запущенную оператором S2VNA не закрывает; файлы настроек S2VNA не правит
  (решение пользователя от 2026-10-16, `src/ui/S2VnaRuntime`).
- Набор SCPI закрыт контрактом `docs/contracts/vna-c2220-scpi.md`. Новая
  команда — сначала в контракт, потом в код. `SYSTem:RECeiver:DIRect:ACCess`
  программа сама не включает.
- Протокола контроллера изделия нет (т. 14 ТЗ). Не придумывать кадры, CRC,
  readback, тайминги и safe-state. До получения документации реальный режим
  калибровки 64×64 остаётся заблокированным; `StubDutController` — заглушка.
- GUI не шлёт команд оборудованию: всё через `MeasurementOrchestrator`.
  Автомат состояний — ровно по т. 8.4 (`docs/contracts/cpp-interfaces.md` §8),
  новые рёбра без решения пользователя не добавлять.
- Имена в `include/afar/` совпадают с т. 8.3 ТЗ посимвольно; интерфейсы не
  расширять «на будущее».
- Завершённые слоты `raw-s21.h5` не перезаписываются. Демоданные помечаются
  «ДЕМО / НЕ ДЛЯ МЕТРОЛОГИИ» в GUI, `.s2p` и PDF.
- Боевые IP стенда, пароли — не в git.
- Не трогай `build/`, `dist/` и сгенерированные `*_autogen/`.
- `git commit`, `git push`, смена ветки — только по явной просьбе
  (правило `.claude/rules/git-workflow.md`).

## Конфигурация агентов

Перенесено из репозитория `V1DeeK/RMO` (`.claude/`) и адаптировано под этот
проект. Старый `.cursor/` не тронут. Конвенции для скиллов и субагентов —
`.claude/config.json`.

- `.claude/rules/` — правила. `cpp-qt-style`, `testing`, `security`,
  `documentation` подгружаются по `paths`; `commit-messages` и `git-workflow`
  действуют всегда.
- `.claude/agents/` — 11 субагентов. Субагент не видит этот чат: всё нужное
  передаётся в задании. `hw-contracts` ведёт контракты SCPI и контроллера
  изделия (аналог `protocolist` из RMO).
- `.claude/skills/` — `/implement`, `/orchestrate`, `/refactor`,
  `/review-changes`, `/audit` и модули знаний (`build-and-test`, `git-helper`,
  `task-management`, `docs`, `architecture-principles`,
  `code-quality-standards`, `security-guidelines`).
- `.claude/settings.json` + `.claude/hooks/guard-shell.py` — перед
  `git push --force`, `git reset --hard`, `git rebase`, `git clean -f`,
  рекурсивным удалением Claude спрашивает подтверждение.
- `.claude/workspace/` — файлы состояния оркестрации, не коммитятся.

### Кого звать

| Задача | Как |
|---|---|
| правка в 1–3 файлах | `/implement` → `worker`/`hw-contracts` → `test-runner` → `reviewer` |
| фича на несколько модулей, SCPI или контроллер | `/orchestrate` → `planner` → по задачам → аудит → `documenter` |
| код работает, но неудобен | `/refactor` → `senior-reviewer` → `refactorer` → `test-runner` |
| перед коммитом | `/review-changes` → `reviewer` (+ `security-auditor` для `vna/`, `dut/`, `data/`, `report/`) |
| перед выпуском | `/audit` → три ревьюера параллельно → `test-runner` → отчёт |

`reviewer`, `senior-reviewer`, `security-auditor` — только чтение (без Edit/Write).
