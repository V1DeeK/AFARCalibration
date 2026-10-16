# Обмен с изделием по Ethernet — протокол v8

Источник: `structural_scheme_v8.docx` (получен от пользователя 16.10.2026).
Этот контракт заменяет `ps-json-protocol.md` для текущей реализации.

## Каналы

- Управление: TCP 49600, по одному объекту JSON в строке, завершённой `\n`.
  Первая команда после соединения — `hello` с `ver`, `tlm_ip`, `tlm_port`.
- Техническое состояние: UDP на адрес и порт из `hello`, один пакет ровно
  10 байт раз в секунду. Это единственная UDP-телеметрия, не JSON. По
  уточнению пользователя бит 0 — младший бит байта 0, бит 8 — младший бит
  байта 1, и так далее. Биты 0–79 описаны таблицей 5 документа.
  У пакета нет номера, поэтому определить число потерянных пакетов нельзя.
- IQ: `libiio` на TCP 30431, устройство `cf-ad9361-lpc`, четыре канала
  `voltage0`–`voltage3` (I1, Q1, I2, Q2), `le:S12/16>>0`. Только чтение и
  только в режиме `SERVICE`; параметры тракта через libiio не записывать.

## Команды управления

Обёртка JSON: `msg_type`, `msg_class`, `msg_count` 0–255, `payload`.
Ответ повторяет тип и счётчик команды. Статусы: `OK`, `BUSY`,
`NOT_SUPPORTED`, `INVALID_ARGUMENT`, `HW_NOT_READY`.

Режимы: `IDLE`, `FULL_SCAN_FREQ`, `OPERATIONAL`, `SERVICE`. Вход в
`SERVICE` из `IDLE/READY`. Режим узнаётся командой `get_state`, не из UDP.

Для калибровки в `SERVICE`:

- `set_rfm_params` / `get_rfm_params`: ровно 16 элементов `ch` с уникальными
  `n` от 1 до 16, `ph` и `att` от 0 до 63, `en` 0 или 1. Ответ содержит
  коды, считанные после записи.
- `get_rfm_telemetry`: `temp` — четыре температуры РЧ-модулей.
- `set_lo_params` / `get_lo_params`, `set_mixer_params` /
  `get_mixer_params`, `set_video_params` / `get_video_params` — управление
  внутренними модулями. Подробные поля и диапазоны: таблицы 28–33.
- `get_trx_status` и `get_pl_status` — состояние трансивера и ПЛИС.
- `set_beam_azimuth`, `set_beam`, `get_beam` — луч; `get_beam.src` принимает
  `AZIMUTH`, `TABLE`, `CODES`, `NONE`.
- `rs485_send` — передача HEX-строки с ответными байтами в `data`.

Команды `set_adc`, `get_adc`, `set_phase_atten`, `get_phase_atten`, `set_lo`,
`get_lo`, `set_mixer`, `get_mixer`, `set_video`, `get_video` не входят в v8.

Для `FULL_SCAN_FREQ`: `set_sff_parameters` принимает `freq_min`,
`freq_max` в пределах 4,9–6,0 ГГц, `az_min`, `az_max` в пределах ±30°.
Параметра `period` нет. Результат `send_sff_result` — событие с
`freq`, `cal` и массивом `fft`.
