#pragma once

#include <cstdint>
#include <string>

/// Абстракция SCPI-кадра: одна строка команды/ответа, завершённая `\n`.
/// Socket и COM — отдельные реализации; не смешивать в одном классе.
class IScpiTransport {
public:
    virtual ~IScpiTransport() = default;

    virtual void connect() = 0;
    virtual void disconnect() noexcept = 0;
    virtual bool is_connected() const noexcept = 0;

    /// Отправить команду; транспорт добавляет `\n`, если его нет.
    virtual void write_line(const std::string& line) = 0;

    /// Читать до `\n` (символ перевода строки в результат не входит).
    virtual std::string read_line() = 0;

    /// Прервать ожидание I/O и закрыть канал (без исключений).
    virtual void abort() noexcept = 0;

    virtual void set_io_timeout_ms(std::uint32_t timeout_ms) = 0;
};
