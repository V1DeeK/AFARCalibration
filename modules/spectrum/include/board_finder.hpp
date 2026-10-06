#pragma once

// Поиск платы (Zynq-7020 + FMCOMMS) в локальной сети по её MAC-адресу.
//
// Адрес плате выдаёт DHCP, и он меняется между загрузками, а MAC постоянный —
// поэтому опознаём именно по MAC. Тот же приём, что в scripts/find-board.sh,
// только без внешних утилит: рассылаем UDP-датаграммы по всей подсети, чтобы
// ядро само разрешило ARP для каждого адреса, и затем читаем таблицу соседей.
// UDP выбран вместо ICMP намеренно: ping требует прав (raw-сокет) либо внешнего
// бинарника, а обычная датаграмма отправляется из-под любого пользователя.

#include <string>

#include "app_state.hpp"

struct BoardFindResult {
    bool        found = false;
    std::string ip;     // заполнен только при found
    std::string status; // человекочитаемое пояснение для UI (и при успехе, и при неудаче)
};

// Ищет MAC вида "42:7e:d7:4e:0f:77" (регистр не важен, разделитель ':' или '-').
// Блокирует вызывающий поток на время опроса подсети — секунды.
BoardFindResult find_board_by_mac(const std::string& mac_text);

// Отдельный поток, чтобы опрос подсети не тормозил ни захват, ни отрисовку:
// ждёт board_find_requested, кладёт результат в AppState.
class BoardFinderThread {
public:
    explicit BoardFinderThread(AppState& state) : state_(state) {}
    void run();

private:
    AppState& state_;
};
