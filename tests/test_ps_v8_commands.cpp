// Проверка по пунктам протокола v8 (structural_scheme_v8.docx): на каждую команду, которую
// программа шлёт PS, — свой тест. Команда отправляется так же, как это делает оператор
// (кнопкой на вкладке), тестовый PS записывает, что пришло, а тест сверяет msg_type и
// payload с таблицами документа и проверяет, что ответ показан оператору.
// Оперативный режим (п. 3.5) — окошко «Оперативный режим» в верхнем ряду «Настроек».

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "FullScanPanel.h"
#include "OperationalPanel.h"
#include "MacResolver.h"
#include "SettingsTab.h"
#include "PsDiagnosticsPage.h"
#include "PsLink.h"
#include "PsModulesPage.h"
#include "PsOsdPage.h"
#include "PsSession.h"
#include "TestPsServer.h"
#include "TestRunTab.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QTableWidget>
#include <QUdpSocket>
#include <QtTest/QtTest>

#include <functional>
#include <memory>

namespace ps = afar::ps;
using nlohmann::json;

namespace {

QApplication& app()
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    static int argc = 1;
    static char name[] = "test_ps_v8_commands";
    static char* argv[] = {name, nullptr};
    // Не разрушаем до выхода процесса — см. qt_test_application.h.
    static QApplication& a = *new QApplication(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("AFAR-tests"));
    QCoreApplication::setApplicationName(QStringLiteral("test_ps_v8_commands"));
    return a;
}

bool waitFor(const std::function<bool()>& cond, int timeoutMs = 3000)
{
    QElapsedTimer t;
    t.start();
    while (!cond()) {
        if (t.elapsed() > timeoutMs) {
            return false;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    return true;
}

template <typename T>
T* child(QWidget& w, const char* name)
{
    T* c = w.findChild<T*>(QString::fromLatin1(name));
    INFO(name);
    REQUIRE(c != nullptr);
    return c;
}

quint16 freeTlmPort()
{
    QUdpSocket probe;
    REQUIRE(probe.bind(QHostAddress::LocalHost, 0));
    const quint16 port = probe.localPort();
    probe.close();
    return port;
}

/// Программа на связи с тестовым PS: вкладки «Подключение», «Установка параметров»,
/// «Тестовый прогон» на одной связи, как в главном окне.
struct Bench {
    Bench()
    {
        app();
        QSettings().clear();
        session = std::make_unique<PsSession>();
        session->macResolver()->setArpReader([] { return QHash<QString, QString>{}; });
        session->macResolver()->setCandidateProvider([] { return QStringList{}; });
        settings = std::make_unique<SettingsTab>(session.get());
        connection = settings.get();
        parameters = settings.get();
        testRun = std::make_unique<TestRunTab>(session.get());
        connection->resize(1300, 800);
        connection->show();
        parameters->show();
        testRun->show();
    }
    ~Bench()
    {
        testRun.reset();
        settings.reset();
        session.reset();
    }

    void connect()
    {
        REQUIRE(server.listen());
        QWidget& c = *connection;
        child<QLineEdit>(c, "psHost")->setText(QStringLiteral("127.0.0.1"));
        child<QLineEdit>(c, "psMac")->setText(QString());
        child<QSpinBox>(c, "psPort")->setValue(server.port());
        child<QSpinBox>(c, "psTlmPort")->setValue(freeTlmPort());
        child<QPushButton>(c, "psConnect")->click();
        REQUIRE(waitFor([&] { return session->link()->isReady(); }));
        REQUIRE(waitFor([&] { return session->modeKnown(); }));
    }

    void toService()
    {
        click("psToService");
        REQUIRE(waitFor([&] { return session->mode() == ps::Mode::Service; }));
        QCoreApplication::processEvents();
    }

    QPushButton* button(const char* name) { return child<QPushButton>(*parameters, name); }

    void click(const char* name)
    {
        QPushButton* b = button(name);
        REQUIRE(waitFor([&] { return b->isEnabled(); }));
        b->click();
    }

    /// Команда пришла в PS; возвращает её payload.
    json received(std::string_view type, int timeoutMs = 3000)
    {
        INFO(std::string(type));
        REQUIRE(waitFor([&] { return server.last(type) != nullptr; }, timeoutMs));
        const ps::Envelope* env = server.last(type);
        CHECK(env->msg_class == ps::MsgClass::Command);
        return env->payload;
    }

    TestPsServer server;
    std::unique_ptr<PsSession> session;
    std::unique_ptr<SettingsTab> settings;
    /// Бывшие вкладки «Подключение» и «Установка параметров» — теперь одна «Настройки».
    SettingsTab* connection = nullptr;
    SettingsTab* parameters = nullptr;
    std::unique_ptr<TestRunTab> testRun;
};

}  // namespace

// ------------------------------------------------------- 3.2 управление изделием

TEST_CASE("v8 3.2.1.1 hello: первая команда, ver/tlm_ip/tlm_port, ответ с ver", "[v8][hello]")
{
    Bench b;
    b.connect();
    REQUIRE_FALSE(b.server.received().empty());
    const ps::Envelope& first = b.server.received().front();
    CHECK(first.msg_type == ps::msg::Hello);
    CHECK(first.payload.at("ver") == std::string(ps::kProtocolVersion));
    CHECK(first.payload.at("tlm_ip").is_string());
    CHECK(first.payload.at("tlm_port").is_number_integer());
    CHECK(b.session->link()->peerVersion() == QString::fromUtf8(ps::kProtocolVersion.data()));
}

TEST_CASE("v8 3.2.1.2 keep_alive: при тишине в канале, payload пустой", "[v8][keep_alive]")
{
    Bench b;
    auto t = b.session->link()->timeouts();
    t.keepAliveIdleMs = 300;
    b.session->link()->setTimeouts(t);
    b.connect();
    const json p = b.received(ps::msg::KeepAlive, 3000);
    CHECK(p == json::object());
    CHECK(b.session->link()->isReady());
}

TEST_CASE("v8 3.2.1.2 keep_alive: кнопка «Отправить keep_alive», payload пустой", "[v8][keep_alive]")
{
    Bench b;
    CHECK_FALSE(b.button("psKeepAliveNow")->isEnabled());
    b.connect();
    b.click("psKeepAliveNow");
    CHECK(b.received(ps::msg::KeepAlive) == json::object());
    CHECK(b.session->link()->isReady());
}

TEST_CASE("v8 3.2.1.3 set_mode: кнопки SERVICE и IDLE, FULL_SCAN_FREQ из полного сканирования", "[v8][set_mode]")
{
    Bench b;
    b.connect();
    b.toService();
    CHECK(b.received(ps::msg::SetMode) == json{{"mode", "SERVICE"}});
    CHECK(b.parameters->findChild<QLabel*>(QStringLiteral("psMode"))->text().contains(QStringLiteral("сервисный")));
    b.click("psToIdle");
    REQUIRE(waitFor([&] { return b.session->mode() == ps::Mode::Idle; }));
    CHECK(b.received(ps::msg::SetMode) == json{{"mode", "IDLE"}});
    b.click("sffApply");
    REQUIRE(waitFor([&] { return b.session->mode() == ps::Mode::FullScanFreq; }));
    CHECK(b.received(ps::msg::SetMode) == json{{"mode", "FULL_SCAN_FREQ"}});
}

TEST_CASE("v8 3.2.1.4 get_state: кнопка, ответ mode и state показан", "[v8][get_state]")
{
    Bench b;
    b.connect();
    const auto before = b.server.received().size();
    b.click("psGetState");
    REQUIRE(waitFor([&] { return b.server.received().size() > before; }));
    CHECK(b.received(ps::msg::GetState) == json::object());
    CHECK(b.parameters->findChild<QLabel*>(QStringLiteral("psMode"))->text().contains(QStringLiteral("готов")));
}

TEST_CASE("v8 3.2.1.5 set_start_test: только в IDLE/READY, payload пустой, ждёт конца самопроверки",
          "[v8][set_start_test]")
{
    Bench b;
    CHECK_FALSE(b.button("psStartTest")->isEnabled());
    b.connect();
    b.click("psStartTest");
    CHECK(b.received(ps::msg::SetStartTest) == json::object());
    CHECK(b.server.selfTests() == 1);
    // Ответ приходит после самопроверки: срок ожидания у этой команды свой.
    CHECK(b.session->link()->timeouts().selfTestMs > b.session->link()->timeouts().commandMs);
    b.toService();
    CHECK_FALSE(b.button("psStartTest")->isEnabled());
}

TEST_CASE("v8 3.2.1.6 set_reset: только в IDLE/READY, после перезапуска снова hello", "[v8][set_reset]")
{
    Bench b;
    CHECK_FALSE(b.button("psReset")->isEnabled());
    b.connect();
    REQUIRE(waitFor([&] { return b.button("psReset")->isEnabled(); }));
    // Кнопка спрашивает подтверждение; без диалога — тот же вызов.
    b.parameters->resetBoard();
    CHECK(b.received(ps::msg::SetReset) == json::object());
    REQUIRE(waitFor([&] { return !b.session->link()->isReady(); }));
    REQUIRE(waitFor([&] { return b.session->link()->isReady(); }, 8000));
    CHECK(b.server.received().back().msg_type != ps::msg::SetReset);
    int hellos = 0;
    for (const auto& env : b.server.received()) {
        hellos += env.msg_type == ps::msg::Hello ? 1 : 0;
    }
    CHECK(hellos == 2);
}

// ---------------------------------------------------------------- 3.3 OSD

TEST_CASE("v8 3.3.1.1 set_osd: в любом режиме, все параметры таблицы 13, ответ с los", "[v8][set_osd]")
{
    Bench b;
    b.connect();  // режим IDLE — set_osd тоже принимается
    QWidget& p = *b.parameters;
    child<QCheckBox>(p, "psOsdEn")->setChecked(true);
    child<QComboBox>(p, "psOsdStd")->setCurrentIndex(1);  // NTSC
    child<QComboBox>(p, "psOsdSrc")->setCurrentIndex(0);  // AUTO
    child<QCheckBox>(p, "psOsdField_cls")->setChecked(false);
    child<QSpinBox>(p, "psOsdHos")->setValue(-32);
    child<QSpinBox>(p, "psOsdVos")->setValue(31);
    b.click("psSetOsd");
    const json sent = b.received(ps::msg::SetOsd);
    CHECK(sent == json::parse(R"({"en":1,"std":"NTSC","src":"AUTO",
                                  "fields":["state","freq","az","snr"],"hos":-32,"vos":31})"));
    REQUIRE(waitFor([&] { return child<QLabel>(p, "psOsdLos")->text() == QStringLiteral("видеосигнал есть"); }));
    // Вне диапазона −32…+31 программа не отправляет.
    ps::OsdParams bad = b.parameters->osdPage()->formParams();
    bad.hos = 32;
    CHECK_FALSE(bad.validate());
}

TEST_CASE("v8 3.3.1.2 get_osd: payload пустой, ответ заполняет форму", "[v8][get_osd]")
{
    Bench b;
    b.connect();
    QWidget& p = *b.parameters;
    child<QSpinBox>(p, "psOsdVos")->setValue(7);
    b.click("psSetOsd");
    b.received(ps::msg::SetOsd);
    child<QSpinBox>(p, "psOsdVos")->setValue(0);
    b.click("psGetOsd");
    CHECK(b.received(ps::msg::GetOsd) == json::object());
    REQUIRE(waitFor([&] { return child<QSpinBox>(p, "psOsdVos")->value() == 7; }));
    CHECK(child<QLabel>(p, "psOsdResult")->text().contains(QStringLiteral("прочитаны")));
}

// ---------------------------------------------------- 3.4 полное сканирование

TEST_CASE("v8 3.4.1.1 set_sff_parameters: 4,9–6,0 ГГц, ±30°, без period", "[v8][sff]")
{
    Bench b;
    b.connect();
    QWidget& p = *b.parameters;
    child<QDoubleSpinBox>(p, "sffFreqFrom")->setValue(4900.0);
    child<QComboBox>(p, "sffFreqToUnit")->setCurrentIndex(1);  // ГГц
    child<QDoubleSpinBox>(p, "sffFreqTo")->setValue(6.0);
    child<QDoubleSpinBox>(p, "sffAzFrom")->setValue(-30.0);
    child<QDoubleSpinBox>(p, "sffAzTo")->setValue(30.0);
    b.click("sffApply");
    const json sent = b.received(ps::msg::SetSffParameters);
    CHECK(sent.at("freq_min") == 4'900'000'000LL);
    CHECK(sent.at("freq_max") == 6'000'000'000LL);
    CHECK(sent.at("az_min").get<double>() == Catch::Approx(-30.0));
    CHECK(sent.at("az_max").get<double>() == Catch::Approx(30.0));
    CHECK_FALSE(sent.contains("period"));
    ps::SffParams out;
    CHECK_FALSE(ps::SffParams::fromJson(json{{"freq_min", 4'800'000'000LL}, {"freq_max", 5'000'000'000LL},
                                              {"az_min", 0}, {"az_max", 10}},
                                        out));
}

TEST_CASE("v8 3.4.1.2 get_sff_parameters: кнопка «Прочитать параметры», ответ на форме", "[v8][sff]")
{
    Bench b;
    b.connect();
    b.click("sffRead");
    CHECK(b.received(ps::msg::GetSffParameters) == json::object());
    REQUIRE(waitFor([&] { return b.session->sffParams().has_value(); }));
    CHECK(b.session->sffParams()->freq_max == b.server.sffParams().freq_max);
}

TEST_CASE("v8 3.4.1.3 set_sff_start и 3.4.2.1 send_sff_result: старт и приём событий", "[v8][sff]")
{
    Bench b;
    b.connect();
    b.click("sffStart");
    CHECK(b.received(ps::msg::SetSffStart) == json::object());
    auto* results = child<QTableWidget>(*b.parameters, "sffResults");
    REQUIRE(waitFor([&] { return results->rowCount() == 3; }));
    CHECK(results->item(0, 1)->text() == QStringLiteral("нет"));  // cal "NO"
    CHECK(results->item(1, 1)->text() == QStringLiteral("есть"));  // cal "OK"
}

TEST_CASE("v8 3.4.1.4 set_sff_stop: кнопка «Стоп» в режиме FULL_SCAN_FREQ", "[v8][sff]")
{
    Bench b;
    b.connect();
    CHECK_FALSE(b.button("sffStop")->isEnabled());
    b.click("sffApply");
    REQUIRE(waitFor([&] { return b.session->mode() == ps::Mode::FullScanFreq; }));
    b.click("sffStop");
    CHECK(b.received(ps::msg::SetSffStop) == json::object());
}

// ------------------------------------------------------ 3.5 оперативный режим

namespace {

/// Изделие в OPERATIONAL по кнопке «Перейти в оперативный».
void toOperational(Bench& b)
{
    // Подсказки кнопок режима: для чего режим, откуда вход и куда выход (v8 п. 1).
    for (const char* name : {"psToService", "psToOperational", "psToIdle"}) {
        const QString tip = child<QPushButton>(*b.parameters, name)->toolTip();
        CHECK(tip.contains(QStringLiteral("Вход:")));
        CHECK(tip.contains(QStringLiteral("Выход:")));
    }
    CHECK(child<QPushButton>(*b.parameters, "psToOperational")->toolTip().contains(QStringLiteral("FULL_SCAN_FREQ")));
    b.click("psToOperational");
    REQUIRE(waitFor([&] { return b.session->mode() == ps::Mode::Operational; }));
    CHECK(b.received(ps::msg::SetMode) == json{{"mode", "OPERATIONAL"}});
    QCoreApplication::processEvents();
}

/// Частота задана, калибровка прошла — КА в OP_IDLE.
void toOpIdle(Bench& b)
{
    toOperational(b);
    b.click("opSetFrequency");
    REQUIRE(waitFor([&] { return b.server.opState() == "OP_IDLE"; }));
}

/// Поиск запущен — КА в OP_SURVEY: здесь принимаются захват, стоп и удержание.
void toOpSurvey(Bench& b)
{
    toOpIdle(b);
    b.click("opStartSurvey");
    REQUIRE(waitFor([&] { return b.server.opState() == "OP_SURVEY"; }));
}

QString opState(Bench& b)
{
    return child<QLabel>(*b.parameters, "opState")->text();
}

}  // namespace

TEST_CASE("v8 3.5: кнопки оперативного режима сами переводят изделие в OPERATIONAL", "[v8][op]")
{
    Bench b;
    CHECK_FALSE(b.button("opGetFrequency")->isEnabled());
    b.connect();
    b.click("opGetFrequency");
    REQUIRE(waitFor([&] { return b.server.last(ps::msg::GetOpFrequency) != nullptr; }));
    CHECK(b.received(ps::msg::SetMode) == json{{"mode", "OPERATIONAL"}});
    CHECK(b.session->mode() == ps::Mode::Operational);
    CHECK(child<QLabel>(*b.parameters, "psMode")->text().contains(QStringLiteral("оперативный")));
}

TEST_CASE("Таблица 1: из SERVICE в OPERATIONAL и FULL_SCAN_FREQ — через IDLE", "[v8][set_mode]")
{
    Bench b;
    b.connect();
    b.toService();
    const auto before = b.server.received().size();
    b.click("opGetFrequency");
    REQUIRE(waitFor([&] { return b.server.last(ps::msg::GetOpFrequency) != nullptr; }));
    std::vector<std::string> modes;
    for (std::size_t i = before; i < b.server.received().size(); ++i) {
        const ps::Envelope& e = b.server.received()[i];
        if (e.msg_type == ps::msg::SetMode) {
            modes.push_back(e.payload.at("mode").get<std::string>());
        }
    }
    CHECK(modes == std::vector<std::string>{"IDLE", "OPERATIONAL"});
    CHECK(b.session->mode() == ps::Mode::Operational);

    b.click("psToIdle");
    REQUIRE(waitFor([&] { return b.session->mode() == ps::Mode::Idle; }));
    b.toService();
    b.click("sffRead");
    REQUIRE(waitFor([&] { return b.server.last(ps::msg::GetSffParameters) != nullptr; }));
    CHECK(b.session->mode() == ps::Mode::FullScanFreq);
}

TEST_CASE("v8 3.5.1.1 set_op_frequency: Гц, 4,9–6,0 ГГц, ответ после калибровки", "[v8][op]")
{
    Bench b;
    b.connect();
    toOperational(b);
    child<QDoubleSpinBox>(*b.parameters, "opFreq")->setValue(4900.0);
    b.click("opSetFrequency");
    CHECK(b.received(ps::msg::SetOpFrequency) == json{{"freq", 4900000000LL}});
    REQUIRE(waitFor([&] { return opState(b).contains(QStringLiteral("set_op_frequency: OK")); }));
    CHECK(b.server.op().at("freq") == 4900000000LL);
    CHECK(child<QLabel>(*b.parameters, "opCal")->text().contains(QStringLiteral("выполнена")));
    // После смены состояния вложенного КА программа сама спрашивает get_state.
    REQUIRE(waitFor([&] { return b.session->state() == QStringLiteral("OP_IDLE"); }));
}

TEST_CASE("v8 3.5.1.2 get_op_frequency: payload пустой, частота на форме", "[v8][op]")
{
    Bench b;
    b.connect();
    toOperational(b);
    b.click("opGetFrequency");
    CHECK(b.received(ps::msg::GetOpFrequency) == json::object());
    auto* freq = child<QDoubleSpinBox>(*b.parameters, "opFreq");
    REQUIRE(waitFor([&] { return freq->value() == Catch::Approx(4900.0); }));
}

TEST_CASE("v8 3.5.1.3 set_op_policy: acq и loss AUTO/CONFIRM", "[v8][op]")
{
    Bench b;
    b.connect();
    toOperational(b);
    auto* acq = child<QComboBox>(*b.parameters, "opAcq");
    auto* loss = child<QComboBox>(*b.parameters, "opLoss");
    acq->setCurrentIndex(acq->findData(QStringLiteral("AUTO")));
    loss->setCurrentIndex(loss->findData(QStringLiteral("CONFIRM")));
    b.click("opSetPolicy");
    CHECK(b.received(ps::msg::SetOpPolicy) == json{{"acq", "AUTO"}, {"loss", "CONFIRM"}});
    REQUIRE(waitFor([&] { return b.server.op().at("loss") == "CONFIRM"; }));
}

TEST_CASE("v8 3.5.1.4 get_op_policy: payload пустой, политика на форме", "[v8][op]")
{
    Bench b;
    b.connect();
    toOperational(b);
    auto* acq = child<QComboBox>(*b.parameters, "opAcq");
    acq->setCurrentIndex(acq->findData(QStringLiteral("CONFIRM")));
    b.click("opGetPolicy");
    CHECK(b.received(ps::msg::GetOpPolicy) == json::object());
    REQUIRE(waitFor([&] { return acq->currentData().toString() == QStringLiteral("AUTO"); }));
}

TEST_CASE("v8 3.5.1.5 set_op_ray: mode SCAN/FIXED, az_min/az_max/az_fix в ±30°", "[v8][op]")
{
    Bench b;
    b.connect();
    toOperational(b);
    auto* mode = child<QComboBox>(*b.parameters, "opRayMode");
    mode->setCurrentIndex(mode->findData(QStringLiteral("SCAN")));
    child<QDoubleSpinBox>(*b.parameters, "opAzMin")->setValue(-15.0);
    child<QDoubleSpinBox>(*b.parameters, "opAzMax")->setValue(10.0);
    child<QDoubleSpinBox>(*b.parameters, "opAzFix")->setValue(0.0);
    b.click("opSetRay");
    CHECK(b.received(ps::msg::SetOpRay) ==
          json{{"mode", "SCAN"}, {"az_min", -15.0}, {"az_max", 10.0}, {"az_fix", 0.0}});
    CHECK(child<QDoubleSpinBox>(*b.parameters, "opAzMin")->minimum() == Catch::Approx(-30.0));
    CHECK(child<QDoubleSpinBox>(*b.parameters, "opAzMax")->maximum() == Catch::Approx(30.0));
}

TEST_CASE("v8 3.5.1.6 get_op_ray: payload пустой, параметры луча на форме", "[v8][op]")
{
    Bench b;
    b.connect();
    toOperational(b);
    auto* mode = child<QComboBox>(*b.parameters, "opRayMode");
    mode->setCurrentIndex(mode->findData(QStringLiteral("FIXED")));
    b.click("opGetRay");
    CHECK(b.received(ps::msg::GetOpRay) == json::object());
    REQUIRE(waitFor([&] { return mode->currentData().toString() == QStringLiteral("SCAN"); }));
}

TEST_CASE("v8 3.5.1.7 set_op_data: AUTO/FPGA/VIDEO", "[v8][op]")
{
    Bench b;
    b.connect();
    toOperational(b);
    auto* data = child<QComboBox>(*b.parameters, "opData");
    data->setCurrentIndex(data->findData(QStringLiteral("VIDEO")));
    b.click("opSetData");
    CHECK(b.received(ps::msg::SetOpData) == json{{"data", "VIDEO"}});
    REQUIRE(waitFor([&] { return b.server.op().at("data") == "VIDEO"; }));
}

TEST_CASE("v8 3.5.1.8 get_op_data: payload пустой, источник на форме", "[v8][op]")
{
    Bench b;
    b.connect();
    toOperational(b);
    auto* data = child<QComboBox>(*b.parameters, "opData");
    data->setCurrentIndex(data->findData(QStringLiteral("FPGA")));
    b.click("opGetData");
    CHECK(b.received(ps::msg::GetOpData) == json::object());
    REQUIRE(waitFor([&] { return data->currentData().toString() == QStringLiteral("AUTO"); }));
}

TEST_CASE("v8 3.5.1.9 set_op_start_calibration: payload пустой, cal в ответе", "[v8][op]")
{
    Bench b;
    b.connect();
    toOperational(b);
    // Таблица 4: калибровка — только из OP_IDLE; до установки частоты PS ответит BUSY.
    b.click("opStartCalibration");
    REQUIRE(waitFor([&] { return opState(b).contains(QStringLiteral("BUSY")); }));
    b.click("opSetFrequency");
    REQUIRE(waitFor([&] { return b.server.opState() == "OP_IDLE"; }));
    b.click("opStartCalibration");
    CHECK(b.received(ps::msg::SetOpStartCalibration) == json::object());
    auto* cal = child<QLabel>(*b.parameters, "opCal");
    REQUIRE(waitFor([&] { return cal->text() == QStringLiteral("калибровка выполнена"); }));
}

TEST_CASE("v8 3.5.1.10 get_op_calibration: payload пустой, cal NO показан", "[v8][op]")
{
    Bench b;
    b.connect();
    toOperational(b);
    b.click("opGetCalibration");
    CHECK(b.received(ps::msg::GetOpCalibration) == json::object());
    auto* cal = child<QLabel>(*b.parameters, "opCal");
    REQUIRE(waitFor([&] { return cal->text() == QStringLiteral("калибровка не выполнена"); }));
}

TEST_CASE("v8 3.5.1.11 set_op_start_survey и 3.5.2.1 send_op_survey_result", "[v8][op]")
{
    Bench b;
    b.connect();
    toOperational(b);
    b.click("opSetFrequency");
    REQUIRE(waitFor([&] { return b.server.opState() == "OP_IDLE"; }));
    b.click("opStartSurvey");
    CHECK(b.received(ps::msg::SetOpStartSurvey) == json::object());
    auto* survey = child<QTableWidget>(*b.parameters, "opSurvey");
    REQUIRE(waitFor([&] { return survey->rowCount() == 2; }));
    CHECK(survey->item(0, 0)->text() == QStringLiteral("12.0°"));
    CHECK(survey->item(0, 3)->text() == QStringLiteral("помеха"));
    CHECK(survey->item(1, 3)->text() == QStringLiteral("БПЛА"));
    REQUIRE(waitFor([&] { return b.session->state() == QStringLiteral("OP_SURVEY"); }));
}

TEST_CASE("v8 3.5.1.12 set_op_start_acquire и 3.5.2.2 send_op_track_result", "[v8][op]")
{
    Bench b;
    b.connect();
    toOperational(b);
    b.click("opSetFrequency");
    REQUIRE(waitFor([&] { return b.server.opState() == "OP_IDLE"; }));
    b.click("opStartSurvey");
    auto* survey = child<QTableWidget>(*b.parameters, "opSurvey");
    REQUIRE(waitFor([&] { return survey->rowCount() == 2; }));
    // Двойной щелчок по найденному источнику подставляет его азимут в «Захват на».
    emit survey->cellDoubleClicked(1, 0);
    CHECK(child<QDoubleSpinBox>(*b.parameters, "opAcquireAz")->value() == Catch::Approx(-7.5));
    b.click("opStartAcquire");
    CHECK(b.received(ps::msg::SetOpStartAcquire) == json{{"az", -7.5}});
    auto* track = child<QTableWidget>(*b.parameters, "opTrack");
    REQUIRE(waitFor([&] { return track->rowCount() == 2; }));
    CHECK(track->item(0, 1)->text() == QStringLiteral("сопровождение"));  // новые сверху
    CHECK(track->item(1, 1)->text() == QStringLiteral("захват"));
    CHECK(track->item(0, 9)->text() == QStringLiteral("FPGA"));
    CHECK(track->item(0, 10)->text() == QStringLiteral("SD"));
}

TEST_CASE("v8 3.5.1.13 set_op_start_loss: из сопровождения в ограниченный поиск", "[v8][op]")
{
    Bench b;
    b.connect();
    toOperational(b);
    b.click("opSetFrequency");
    REQUIRE(waitFor([&] { return b.server.opState() == "OP_IDLE"; }));
    b.click("opStartSurvey");
    REQUIRE(waitFor([&] { return b.server.opState() == "OP_SURVEY"; }));
    b.click("opStartAcquire");
    REQUIRE(waitFor([&] { return b.server.opState() == "OP_TRACK"; }));
    b.click("opStartLoss");
    CHECK(b.received(ps::msg::SetOpStartLoss) == json::object());
    REQUIRE(waitFor([&] { return b.session->state() == QStringLiteral("OP_LIMITED_SURVEY"); }));
}

TEST_CASE("v8 3.5.1.14 set_op_stop: payload пустой, в OP_IDLE", "[v8][op]")
{
    Bench b;
    b.connect();
    toOperational(b);
    b.click("opSetFrequency");
    REQUIRE(waitFor([&] { return b.server.opState() == "OP_IDLE"; }));
    b.click("opStartSurvey");
    REQUIRE(waitFor([&] { return b.server.opState() == "OP_SURVEY"; }));
    b.click("opStop");
    CHECK(b.received(ps::msg::SetOpStop) == json::object());
    REQUIRE(waitFor([&] { return b.session->state() == QStringLiteral("OP_IDLE"); }));
}

TEST_CASE("v8 3.5.1.15 set_op_start_hold: az в ±30°, при FIXED — отказ виден", "[v8][op]")
{
    Bench b;
    b.connect();
    toOpSurvey(b);
    child<QDoubleSpinBox>(*b.parameters, "opHoldAz")->setValue(13.5);
    b.click("opStartHold");
    CHECK(b.received(ps::msg::SetOpStartHold) == json{{"az", 13.5}});
    auto* hold = child<QLabel>(*b.parameters, "opHold");
    REQUIRE(waitFor([&] { return hold->text().contains(QStringLiteral("13.5")); }));

    auto* mode = child<QComboBox>(*b.parameters, "opRayMode");
    mode->setCurrentIndex(mode->findData(QStringLiteral("FIXED")));
    b.click("opSetRay");
    REQUIRE(waitFor([&] { return b.server.op().at("mode") == "FIXED"; }));
    b.click("opStartHold");
    REQUIRE(waitFor([&] { return opState(b).contains(QStringLiteral("NOT_SUPPORTED")); }));
}

TEST_CASE("v8 3.5.1.16 set_op_stop_hold: payload пустой", "[v8][op]")
{
    Bench b;
    b.connect();
    toOpSurvey(b);
    b.click("opStopHold");
    CHECK(b.received(ps::msg::SetOpStopHold) == json::object());
    auto* hold = child<QLabel>(*b.parameters, "opHold");
    REQUIRE(waitFor([&] { return hold->text() == QStringLiteral("удержание: выключено"); }));
    CHECK_FALSE(b.server.op().at("hold").get<bool>());
}

// ------------------------------------------------- 3.6 инструментальный режим

TEST_CASE("v8 3.6.1.1 rs485_send: только SERVICE, HEX-строка, ответные байты показаны", "[v8][rs485]")
{
    Bench b;
    b.connect();
    QWidget& t = *b.testRun;
    auto* send = child<QPushButton>(t, "psRawSend");
    CHECK_FALSE(send->isEnabled());
    b.toService();
    REQUIRE(waitFor([&] { return send->isEnabled(); }));
    child<QLineEdit>(t, "psRawHex")->setText(QStringLiteral("01 a3 ff"));
    send->click();
    CHECK(b.received(ps::msg::Rs485Send) == json{{"data", "01A3FF"}});
    REQUIRE(waitFor([&] { return child<QLabel>(t, "psRawResult")->text().contains(QStringLiteral("01A3FF")); }));
    // Нечётная длина не уходит.
    const auto before = b.server.received().size();
    child<QLineEdit>(t, "psRawHex")->setText(QStringLiteral("01A"));
    send->click();
    CHECK(b.server.received().size() == before);
}

TEST_CASE("v8 3.6.1.2 set_rfm_params: ровно 16 каналов n/ph/att/en, ответ — коды из микросхем",
          "[v8][rfm]")
{
    Bench b;
    b.connect();
    CHECK_FALSE(b.button("psWriteCodes")->isEnabled());
    b.toService();
    auto* codes = child<QTableWidget>(*b.parameters, "psCodes");
    static_cast<QSpinBox*>(codes->cellWidget(0, 1))->setValue(63);
    static_cast<QSpinBox*>(codes->cellWidget(0, 2))->setValue(5);
    static_cast<QSpinBox*>(codes->cellWidget(15, 3))->setValue(0);
    b.click("psWriteCodes");
    const json sent = b.received(ps::msg::SetPhaseAtten);
    REQUIRE(sent.at("ch").size() == 16);
    for (int i = 0; i < 16; ++i) {
        const json& c = sent["ch"][static_cast<std::size_t>(i)];
        CHECK(c.at("n") == i + 1);
        CHECK(c.contains("ph"));
        CHECK(c.contains("att"));
        CHECK(c.contains("en"));
    }
    CHECK(sent["ch"][0] == json{{"n", 1}, {"ph", 63}, {"att", 5}, {"en", 1}});
    CHECK(sent["ch"][15]["en"] == 0);
    REQUIRE(waitFor([&] { return codes->item(0, 4)->text() == QStringLiteral("63"); }));
    CHECK(codes->item(15, 6)->text() == QStringLiteral("0"));
    // 15 каналов или код 64 программа не отправляет.
    json payload;
    std::vector<ps::ChannelCodes> fifteen(15);
    for (int i = 0; i < 15; ++i) {
        fifteen[static_cast<std::size_t>(i)] = {i + 1, 0, 0, 1};
    }
    CHECK_FALSE(ps::channelsToJson(fifteen, payload));
}

TEST_CASE("v8 3.6.1.3 get_rfm_params: payload пустой, 16 каналов в таблице", "[v8][rfm]")
{
    Bench b;
    b.connect();
    b.toService();
    b.click("psReadCodes");
    CHECK(b.received(ps::msg::GetPhaseAtten) == json::object());
    auto* codes = child<QTableWidget>(*b.parameters, "psCodes");
    REQUIRE(waitFor([&] { return codes->item(15, 4)->text() != QStringLiteral("—"); }));
}

TEST_CASE("v8 3.6.1.4 get_rfm_telemetry: четыре температуры РЧ-модулей", "[v8][rfm]")
{
    Bench b;
    b.connect();
    CHECK_FALSE(b.button("psGetRfmTelemetry")->isEnabled());
    b.toService();
    b.click("psGetRfmTelemetry");
    CHECK(b.received(ps::msg::GetRfmTelemetry) == json::object());
    QTableWidget* table = b.parameters->diagnosticsPage()->table();
    REQUIRE(waitFor([&] { return table->rowCount() == 4; }));
    CHECK(table->item(0, 1)->text() == QStringLiteral("Температура РЧ-модуля 1"));
    CHECK(table->item(3, 2)->text() == QStringLiteral("37.4 °C"));
}

namespace {

void setLoField(QWidget& p, const QString& prefix, const char* key, double value)
{
    const QString name = prefix + QLatin1Char('_') + QString::fromLatin1(key);
    QWidget* editor = p.findChild<QWidget*>(name);
    INFO(name.toStdString());
    REQUIRE(editor != nullptr);
    if (auto* box = qobject_cast<QCheckBox*>(editor)) {
        box->setChecked(value != 0.0);
    } else if (auto* combo = qobject_cast<QComboBox*>(editor)) {
        combo->setCurrentIndex(combo->findData(static_cast<int>(value)));
    } else if (auto* spin = qobject_cast<QDoubleSpinBox*>(editor)) {
        spin->setValue(value);
    }
    p.findChild<QCheckBox*>(name + QStringLiteral("_send"))->setChecked(true);
}

}  // namespace

TEST_CASE("v8 3.6.1.5 set_lo_params: addr 1 и 2, поля таблицы 28, ответ с temp и lock", "[v8][lo]")
{
    Bench b;
    b.connect();
    b.toService();
    QWidget& p = *b.parameters;
    setLoField(p, QStringLiteral("psLo1"), "pwr_b", -4);
    setLoField(p, QStringLiteral("psLo1"), "en_syn", 1);
    setLoField(p, QStringLiteral("psLo1"), "ref", 1);
    setLoField(p, QStringLiteral("psLo1"), "freq", 5800.0);  // МГц на форме
    setLoField(p, QStringLiteral("psLo1"), "att2", 31.5);
    b.click("psLo1Write");
    CHECK(b.received(ps::msg::SetLo) ==
          json{{"addr", 1}, {"pwr_b", -4}, {"en_syn", 1}, {"ref", 1}, {"freq", 5'800'000'000LL}, {"att2", 31.5}});
    REQUIRE(waitFor([&] { return child<QLabel>(p, "psLo1State")->text().contains(QStringLiteral("t =")); }));
    CHECK(child<QLabel>(p, "psLo1State")->text().contains(QStringLiteral("захват")));

    setLoField(p, QStringLiteral("psLo2"), "en_rf", 0);
    b.click("psLo2Write");
    REQUIRE(waitFor([&] { return b.server.last(ps::msg::SetLo)->payload.at("addr") == 2; }));
    // Шаг 0,5 дБ и мощности −4/−1/2/5: остальное не отправляется.
    json payload;
    CHECK_FALSE(ps::loSetPayload(1, {{"att1", 0.3}}, payload));
    CHECK_FALSE(ps::loSetPayload(1, {{"pwr_a", 3}}, payload));
    CHECK_FALSE(ps::loSetPayload(3, {{"en_rf", 1}}, payload));
}

TEST_CASE("v8 3.6.1.6 get_lo_params: только addr, ответ — все поля таблиц 28 и 29", "[v8][lo]")
{
    Bench b;
    b.connect();
    b.toService();
    b.click("psLo2Read");
    CHECK(b.received(ps::msg::GetLo) == json{{"addr", 2}});
    REQUIRE(waitFor([&] { return child<QLabel>(*b.parameters, "psLo2State")->text().contains(QStringLiteral("OK")); }));
}

TEST_CASE("v8 3.6.1.7 set_mixer_params: 1–4 канала с n, en/rp/if_att/att, ответ с temp", "[v8][mixer]")
{
    Bench b;
    b.connect();
    b.toService();
    QWidget& p = *b.parameters;
    child<QDoubleSpinBox>(p, "psMixer2_if_att")->setValue(15.5);
    child<QDoubleSpinBox>(p, "psMixer2_att")->setValue(10.0);
    child<QCheckBox>(p, "psMixer2_rp")->setChecked(true);
    child<QCheckBox>(p, "psMixer2_en")->setChecked(true);
    child<QCheckBox>(p, "psMixer2_send")->setChecked(true);
    b.click("psMixerWrite");
    CHECK(b.received(ps::msg::SetMixer) ==
          json{{"ch", json::array({json{{"n", 2}, {"en", 1}, {"rp", 1}, {"if_att", 15.5}, {"att", 10.0}}})}});
    REQUIRE(waitFor([&] { return child<QLabel>(p, "psMixerState")->text().contains(QStringLiteral("t =")); }));
    json payload;
    CHECK_FALSE(ps::mixerSetPayload({{5, {{"en", 1}}}}, payload));
    CHECK_FALSE(ps::mixerSetPayload({{1, {{"if_att", 16.0}}}}, payload));
}

TEST_CASE("v8 3.6.1.8 get_mixer_params: payload пустой, 4 канала в ответе", "[v8][mixer]")
{
    Bench b;
    b.connect();
    b.toService();
    b.click("psMixerRead");
    CHECK(b.received(ps::msg::GetMixer) == json::object());
    REQUIRE(waitFor([&] { return child<QLabel>(*b.parameters, "psMixerState")->text().contains(QStringLiteral("OK")); }));
}

TEST_CASE("v8 3.6.1.9 set_video_params: поля таблиц 28 и 32 без addr, ответ temp/lock/pres", "[v8][video]")
{
    Bench b;
    b.connect();
    b.toService();
    QWidget& p = *b.parameters;
    setLoField(p, QStringLiteral("psVideo"), "en_vid", 1);
    setLoField(p, QStringLiteral("psVideo"), "att_mode", 1);
    setLoField(p, QStringLiteral("psVideo"), "freq", 5800.0);
    b.click("psVideoWrite");
    const json sent = b.received(ps::msg::SetVideo);
    CHECK(sent == json{{"en_vid", 1}, {"att_mode", 1}, {"freq", 5'800'000'000LL}});
    CHECK_FALSE(sent.contains("addr"));
    REQUIRE(waitFor([&] { return child<QLabel>(p, "psVideoState")->text().contains(QStringLiteral("видеосигнала")); }));
}

TEST_CASE("v8 3.6.1.10 get_video_params: payload пустой, показан уровень видеосигнала pres", "[v8][video]")
{
    Bench b;
    b.connect();
    b.toService();
    b.click("psVideoRead");
    CHECK(b.received(ps::msg::GetVideo) == json::object());
    REQUIRE(waitFor([&] {
        return child<QLabel>(*b.parameters, "psVideoState")->text().contains(QStringLiteral("уровень видеосигнала"));
    }));
    ps::ModuleState s;
    REQUIRE(ps::ModuleState::fromJson(json{{"temp", 30.0}, {"lock", 1}, {"pres", 0}}, ps::videoFields(), s));
    CHECK(s.pres == 0);
}

TEST_CASE("v8 3.6.1.11 set_osd_text: только SERVICE, row/col/text по таблице 34", "[v8][osd_text]")
{
    Bench b;
    b.connect();
    CHECK_FALSE(b.button("psSetOsdText")->isEnabled());
    b.toService();
    QWidget& p = *b.parameters;
    child<QSpinBox>(p, "psOsdRow")->setValue(15);
    child<QSpinBox>(p, "psOsdCol")->setValue(0);
    child<QLineEdit>(p, "psOsdText")->setText(QStringLiteral("TEST LO 5800 MHZ"));
    b.click("psSetOsdText");
    CHECK(b.received(ps::msg::SetOsdText) == json{{"row", 15}, {"col", 0}, {"text", "TEST LO 5800 MHZ"}});
    REQUIRE(waitFor([&] { return child<QLabel>(p, "psOsdResult")->text().contains(QStringLiteral("выведен")); }));
    // NTSC — 13 строк.
    child<QComboBox>(p, "psOsdStd")->setCurrentIndex(1);
    CHECK(child<QSpinBox>(p, "psOsdRow")->maximum() == 12);
}

TEST_CASE("v8 3.6.1.12 clear_osd_text: только SERVICE, payload пустой", "[v8][osd_text]")
{
    Bench b;
    b.connect();
    CHECK_FALSE(b.button("psClearOsdText")->isEnabled());
    b.toService();
    b.click("psClearOsdText");
    CHECK(b.received(ps::msg::ClearOsdText) == json::object());
    REQUIRE(waitFor([&] { return child<QLabel>(*b.parameters, "psOsdResult")->text().contains(QStringLiteral("очищен")); }));
}

TEST_CASE("v8 3.6.1.13 set_beam_azimuth: az в пределах ±30°", "[v8][beam]")
{
    Bench b;
    b.connect();
    b.toService();
    child<QDoubleSpinBox>(*b.parameters, "psBeamAz")->setValue(-12.5);
    b.click("psSetAzimuth");
    CHECK(b.received(ps::msg::SetBeamAzimuth).at("az").get<double>() == Catch::Approx(-12.5));
    CHECK(child<QDoubleSpinBox>(*b.parameters, "psBeamAz")->minimum() == Catch::Approx(-30.0));
    REQUIRE(waitFor([&] { return child<QLabel>(*b.parameters, "psBeamState")->text().contains(QStringLiteral("азимут")); }));
}

TEST_CASE("v8 3.6.1.14 set_beam: номер луча из таблицы", "[v8][beam]")
{
    Bench b;
    b.connect();
    b.toService();
    child<QSpinBox>(*b.parameters, "psBeamId")->setValue(7);
    b.click("psSetBeam");
    CHECK(b.received(ps::msg::SetBeam) == json{{"beam", 7}});
    REQUIRE(waitFor([&] { return child<QLabel>(*b.parameters, "psBeamState")->text().contains(QStringLiteral("№ 7")); }));
}

TEST_CASE("v8 3.6.1.15 get_beam: src AZIMUTH/TABLE/CODES/NONE заглавными", "[v8][beam]")
{
    Bench b;
    b.connect();
    b.toService();
    b.click("psGetBeam");
    CHECK(b.received(ps::msg::GetBeam) == json::object());
    REQUIRE(waitFor([&] { return child<QLabel>(*b.parameters, "psBeamState")->text().contains(QStringLiteral("не задавался")); }));
    ps::BeamState s;
    CHECK(ps::BeamState::fromJson(json{{"src", "TABLE"}, {"beam", 3}}, s));
    CHECK(s.src == ps::BeamSource::Table);
    CHECK_FALSE(ps::BeamState::fromJson(json{{"src", "azimuth"}, {"az", 1.0}}, s));
}

TEST_CASE("v8 3.6.1.16 get_trx_status: флаги таблицы 36 и температура кристалла", "[v8][trx]")
{
    Bench b;
    b.connect();
    CHECK_FALSE(b.button("psGetTrxStatus")->isEnabled());
    b.toService();
    b.click("psGetTrxStatus");
    CHECK(b.received(ps::msg::GetTrxStatus) == json::object());
    QTableWidget* table = b.parameters->diagnosticsPage()->table();
    REQUIRE(waitFor([&] { return table->rowCount() == 9; }));
    CHECK(table->item(0, 1)->text() == QStringLiteral("Обмен с трансивером по SPI"));
    CHECK(table->item(8, 2)->text() == QStringLiteral("41.5 °C"));
}

TEST_CASE("v8 3.6.1.16 get_trx_status: опрос AD9361 по таймеру, только в SERVICE", "[v8][trx]")
{
    Bench b;
    b.connect();
    auto* page = b.parameters->diagnosticsPage();
    page->setPolling(true, 1);
    CHECK(child<QCheckBox>(*b.parameters, "psPollTrx")->isChecked());
    // Вне сервисного режима опрос ждёт: PS get_trx_status не примет.
    QTest::qWait(1300);
    CHECK(b.server.last(ps::msg::GetTrxStatus) == nullptr);
    b.toService();
    REQUIRE(waitFor([&] { return b.server.last(ps::msg::GetTrxStatus) != nullptr; }, 3000));
    const auto first = b.server.received().size();
    REQUIRE(waitFor([&] {
        int n = 0;
        for (std::size_t i = first; i < b.server.received().size(); ++i) {
            n += b.server.received()[i].msg_type == ps::msg::GetTrxStatus ? 1 : 0;
        }
        return n >= 1;
    }, 3000));
    REQUIRE(waitFor([&] { return page->table()->rowCount() == 9; }));
    page->setPolling(false);
    CHECK_FALSE(page->polling());
    CHECK_FALSE(child<QCheckBox>(*b.parameters, "psPollTrx")->isChecked());
}

TEST_CASE("v8 3.6.1.17 get_pl_status: флаги таблицы 37", "[v8][pl]")
{
    Bench b;
    b.connect();
    CHECK_FALSE(b.button("psGetPlStatus")->isEnabled());
    b.toService();
    b.click("psGetPlStatus");
    CHECK(b.received(ps::msg::GetPlStatus) == json::object());
    QTableWidget* table = b.parameters->diagnosticsPage()->table();
    REQUIRE(waitFor([&] { return table->rowCount() == 3; }));
    CHECK(table->item(0, 1)->text() == QStringLiteral("Поток данных от приёмопередатчика"));
    CHECK(table->item(1, 2)->text().contains(QStringLiteral("ошибок нет")));
}

// --------------------------------------------- 2 канал технического состояния

TEST_CASE("Таблица 5: 10 байт UDP, 1 — исправно, биты 14–15 — резерв", "[v8][udp]")
{
    Bench b;
    // FF FF FF FF FF FF FF FF FF FF — всё исправно.
    b.server.setStatusBytes(QByteArray(10, char(0xff)));
    b.connect();
    REQUIRE(waitFor([&] { return b.session->telemetry().has_value(); }));
    for (const auto& [name, health] : b.session->telemetry()->devices) {
        INFO(name);
        CHECK(health == ps::DeviceHealth::Ok);
    }

    // Бит 8 «связь» = 0 и бит 13 «проверка канала данных» = 0 — неисправность;
    // бит 14 = 0 — резерв, его нет в списке.
    ps::Telemetry t;
    std::string raw(10, char(0xff));
    raw[1] = static_cast<char>(0xff & ~0x01u & ~0x20u & ~0x40u);
    REQUIRE(ps::Telemetry::fromStatusBytes(raw, t));
    const auto health = [&](const std::string& name) {
        for (const auto& [n, h] : t.devices) {
            if (n == name) {
                return h;
            }
        }
        FAIL("нет флага " << name);
        return ps::DeviceHealth::NotChecked;
    };
    CHECK(health("Трансивер: связь") == ps::DeviceHealth::Fail);
    CHECK(health("Трансивер: проверка данных") == ps::DeviceHealth::Fail);
    CHECK(health("Трансивер: канал данных АЦП") == ps::DeviceHealth::Ok);
    for (const auto& [n, h] : t.devices) {
        CHECK(n.find("уровень входного сигнала") == std::string::npos);
        CHECK(n.find("приём данных в память") == std::string::npos);
    }
    CHECK(health("Самопроверка: калибровочные таблицы") == ps::DeviceHealth::Ok);
    CHECK_FALSE(ps::Telemetry::fromStatusBytes(std::string(9, '\0'), t));
}
