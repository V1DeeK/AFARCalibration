#include "StubDutController.h"

#include <stdexcept>
#include <string>

namespace {

[[noreturn]] void throw_protocol_not_provided(const char* op)
{
    throw std::runtime_error(
        std::string("StubDutController::") + op
        + ": протокол контроллера АФАР не передан (т. 14 ТЗ); "
          "COM/TCP не открываются");
}

}  // namespace

void StubDutController::connect()
{
    throw_protocol_not_provided("connect");
}

void StubDutController::apply(const DutState&)
{
    throw_protocol_not_provided("apply");
}

DutState StubDutController::readback()
{
    throw_protocol_not_provided("readback");
}

double StubDutController::temperature_c()
{
    throw_protocol_not_provided("temperature_c");
}

void StubDutController::set_safe_state() noexcept
{
    // Нет связи с изделием — безопасный no-op без I/O.
}
