#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "AcceptanceAtTab.h"
#include "ConnectionBar.h"
#include "MeasureTab.h"
#include "S21PlotWidget.h"
#include "S2VnaRuntime.h"
#include "StartWizard.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPointF>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStandardItemModel>
#include <QTemporaryDir>
#include <QToolButton>
#include <QGroupBox>
#include <QListWidget>
#include <QWheelEvent>

namespace {

QApplication& application()
{
    static int argc = 1;
    static char name[] = "test_ui_interactions";
    static char* argv[] = {name, nullptr};
    static QApplication app(argc, argv);
    app.setOrganizationName(QStringLiteral("AFAR Tests"));
    app.setApplicationName(QStringLiteral("AFAR UI Interactions Tests"));
    return app;
}

}  // namespace

TEST_CASE("S2VNA socket flag is enabled without losing hardware settings", "[ui][s2vna]")
{
    QTemporaryDir temporary;
    REQUIRE(temporary.isValid());
    QDir root(temporary.path());
    REQUIRE(root.mkpath(QStringLiteral("System")));
    QFile executable(root.filePath(QStringLiteral("S2VNA.exe")));
    REQUIRE(executable.open(QIODevice::WriteOnly));
    executable.close();
    const QString setupPath = root.filePath(QStringLiteral("System/Setup.dat"));
    {
        QSettings setup(setupPath, QSettings::IniFormat);
        setup.setValue(QStringLiteral("SocketSvrSetup/SocketSvrEnabled"), 0);
        setup.setValue(QStringLiteral("Hardware/DeviceID"), 23);
    }

    QString error;
    REQUIRE(S2VnaRuntime::enableSocketServer(executable.fileName(), &error));
    QSettings setup(setupPath, QSettings::IniFormat);
    REQUIRE(setup.value(QStringLiteral("SocketSvrSetup/SocketSvrEnabled")).toInt() == 1);
    REQUIRE(setup.value(QStringLiteral("Hardware/DeviceID")).toInt() == 23);
}

TEST_CASE("S2VNA demo selects C2220 and keeps unrelated settings", "[ui][s2vna][demo]")
{
    QTemporaryDir temporary;
    REQUIRE(temporary.isValid());
    QDir root(temporary.path());
    REQUIRE(root.mkpath(QStringLiteral("System")));
    QFile executable(root.filePath(QStringLiteral("S2VNA.exe")));
    REQUIRE(executable.open(QIODevice::WriteOnly));
    executable.close();
    const QString setupPath = root.filePath(QStringLiteral("System/Setup.dat"));
    {
        QSettings setup(setupPath, QSettings::IniFormat);
        setup.setValue(QStringLiteral("SocketSvrSetup/SocketSvrEnabled"), 0);
        setup.setValue(QStringLiteral("Hardware/DeviceID"), 13);
        setup.setValue(QStringLiteral("Display/Theme"), QStringLiteral("dark"));
    }

    QString error;
    REQUIRE(S2VnaRuntime::enableC2220DemoMode(executable.fileName(), &error));
    QSettings setup(setupPath, QSettings::IniFormat);
    REQUIRE(setup.value(QStringLiteral("SocketSvrSetup/SocketSvrEnabled")).toInt() == 1);
    REQUIRE(setup.value(QStringLiteral("Hardware/DeviceID")).toInt() == 23);
    REQUIRE(setup.value(QStringLiteral("Display/Theme")).toString() == QStringLiteral("dark"));
}

TEST_CASE("S2VNA shutdown leaves processes not started by AFAR alone", "[ui][s2vna]")
{
    QString error = QStringLiteral("stale error");
    REQUIRE(S2VnaRuntime::stopStartedProcess(&error));
    REQUIRE(error.isEmpty());
}

TEST_CASE("sweep parameters are saved after an operator edit", "[ui][settings]")
{
    (void)application();
    QSettings settings;
    settings.remove(QStringLiteral("sweep"));

    MeasureTab tab;
    tab.applyRunConfigDefaults(1.20e9, 1.40e9, 501, 2000, -15.0, 4);
    auto* averages = tab.findChild<QSpinBox*>(QStringLiteral("sweepAverages"));
    REQUIRE(averages != nullptr);
    averages->setValue(5);

    REQUIRE(settings.value(QStringLiteral("sweep/f_start_hz")).toDouble()
            == Catch::Approx(1.20e9));
    REQUIRE(settings.value(QStringLiteral("sweep/f_stop_hz")).toDouble()
            == Catch::Approx(1.40e9));
    REQUIRE(settings.value(QStringLiteral("sweep/points")).toInt() == 501);
    REQUIRE(settings.value(QStringLiteral("sweep/ifbw_hz")).toInt() == 2000);
    REQUIRE(settings.value(QStringLiteral("sweep/power_dbm")).toDouble()
            == Catch::Approx(-15.0));
    REQUIRE(settings.value(QStringLiteral("sweep/averages")).toInt() == 5);
    settings.remove(QStringLiteral("sweep"));
}

TEST_CASE("C2220 UI exposes the complete supported sweep range", "[ui][settings]")
{
    MeasureTab tab;
    auto* points = tab.findChild<QSpinBox*>();
    const auto pointSpins = tab.findChildren<QSpinBox*>();
    points = nullptr;
    for (auto* spin : pointSpins) {
        if (spin->minimum() == 2 && spin->maximum() == 500001) {
            points = spin;
            break;
        }
    }
    REQUIRE(points != nullptr);

    auto* start = tab.findChild<QDoubleSpinBox*>(QStringLiteral("frequencyStart"));
    REQUIRE(start != nullptr);
    CHECK(start->maximum() == Catch::Approx(20'000.0)); // текущая единица — МГц
}

TEST_CASE("filter readiness is separate from the series controller", "[ui]")
{
    (void)application();
    ConnectionBar bar;
    bar.setVnaInfo(QStringLiteral("C2220"), QStringLiteral("127.0.0.1:5025"), true);
    bar.setControllerInfo(QStringLiteral("DutSimulator"), false);

    const auto* ready = bar.findChild<QLabel*>(QStringLiteral("filterReadyStatus"));
    const auto* controller = bar.findChild<QLabel*>(QStringLiteral("controllerStatus"));
    REQUIRE(ready != nullptr);
    REQUIRE(controller != nullptr);
    REQUIRE(ready->text().contains(QStringLiteral("ГОТОВО")));
    REQUIRE(controller->text().contains(QStringLiteral("для пассивных S-параметров не нужен")));
    REQUIRE(controller->text().contains(QStringLiteral("CTRL:")));
    REQUIRE_FALSE(controller->text().contains(QStringLiteral("нет связи")));
}

TEST_CASE("work modes distinguish passive demo and unavailable real calibration", "[ui][modes]")
{
    MeasureTab tab;
    auto* modes = tab.findChild<QComboBox*>(QStringLiteral("measurementWorkMode"));
    REQUIRE(modes != nullptr);
    REQUIRE(modes->count() == 3);
    CHECK(modes->itemText(0).contains(QStringLiteral("Двухпортовое")));
    CHECK(modes->itemText(1).contains(QStringLiteral("Демо")));
    CHECK(modes->itemText(2).contains(QStringLiteral("нет контроллера")));

    auto* model = qobject_cast<QStandardItemModel*>(modes->model());
    REQUIRE(model != nullptr);
    REQUIRE(model->item(0) != nullptr);
    CHECK(model->item(0)->isEnabled());
    REQUIRE(model->item(2) != nullptr);
    CHECK_FALSE(model->item(2)->isEnabled());
    CHECK(model->item(2)->toolTip().contains(QStringLiteral("Контроллер изделия отсутствует")));

    ConnectionBar bar;
    auto* controllers = bar.findChild<QComboBox*>(QStringLiteral("controllerBackend"));
    REQUIRE(controllers != nullptr);
    REQUIRE(controllers->findData(0) >= 0);
    const int realRow = controllers->findData(2);
    REQUIRE(realRow >= 0);
    auto* controllerModel = qobject_cast<QStandardItemModel*>(controllers->model());
    REQUIRE(controllerModel != nullptr);
    REQUIRE(controllerModel->item(realRow) != nullptr);
    CHECK_FALSE(controllerModel->item(realRow)->isEnabled());
}

TEST_CASE("VNA timeouts are collapsed until the engineer expands them", "[ui]")
{
    (void)application();
    ConnectionBar bar;
    auto* box = bar.findChild<QGroupBox*>(QStringLiteral("vnaTimeoutsBox"));
    auto* body = bar.findChild<QWidget*>(QStringLiteral("vnaTimeoutsBody"));
    REQUIRE(box != nullptr);
    REQUIRE(body != nullptr);
    REQUIRE_FALSE(box->isChecked());
    REQUIRE(body->isHidden());

    box->setChecked(true);
    REQUIRE_FALSE(body->isHidden());
    box->setChecked(false);
    REQUIRE(body->isHidden());
}

TEST_CASE("connection settings keep the top bar compact until expanded", "[ui]")
{
    (void)application();
    ConnectionBar bar;
    auto* toggle = bar.findChild<QToolButton*>(QStringLiteral("connectionSettingsToggle"));
    auto* body = bar.findChild<QWidget*>(QStringLiteral("connectionSettingsBody"));
    REQUIRE(toggle != nullptr);
    REQUIRE(body != nullptr);
    REQUIRE(body->isHidden());
    toggle->setChecked(true);
    REQUIRE_FALSE(body->isHidden());
    toggle->setChecked(false);
    REQUIRE(body->isHidden());
}

TEST_CASE("S2VNA demo is a distinct local socket backend", "[ui][settings][demo]")
{
    (void)application();
    ConnectionBar bar;
    auto* backend = bar.findChild<QComboBox*>(QStringLiteral("vnaBackend"));
    auto* host = bar.findChild<QLineEdit*>(QStringLiteral("vnaHost"));
    auto* port = bar.findChild<QSpinBox*>(QStringLiteral("vnaPort"));
    auto* com = bar.findChild<QLineEdit*>(QStringLiteral("vnaComPort"));
    REQUIRE(backend != nullptr);
    REQUIRE(backend->findData(3) >= 0);
    backend->setCurrentIndex(backend->findData(3));
    REQUIRE(bar.vnaBackend() == 3);
    REQUIRE(host->isEnabled());
    REQUIRE(port->isEnabled());
    REQUIRE_FALSE(com->isEnabled());
}

TEST_CASE("VNA settings are locked while a series owns the instrument", "[ui][settings]")
{
    (void)application();
    ConnectionBar bar;
    auto* backend = bar.findChild<QComboBox*>(QStringLiteral("vnaBackend"));
    auto* host = bar.findChild<QLineEdit*>(QStringLiteral("vnaHost"));
    auto* port = bar.findChild<QSpinBox*>(QStringLiteral("vnaPort"));
    auto* com = bar.findChild<QLineEdit*>(QStringLiteral("vnaComPort"));
    QPushButton* probe = nullptr;
    for (auto* button : bar.findChildren<QPushButton*>()) {
        if (button->text() == QStringLiteral("Проверить связь")) {
            probe = button;
            break;
        }
    }
    REQUIRE(backend != nullptr);
    REQUIRE(host != nullptr);
    REQUIRE(port != nullptr);
    REQUIRE(com != nullptr);
    REQUIRE(probe != nullptr);

    backend->setCurrentIndex(1);
    bar.setVnaSettingsLocked(true);
    REQUIRE_FALSE(backend->isEnabled());
    REQUIRE_FALSE(host->isEnabled());
    REQUIRE_FALSE(port->isEnabled());
    REQUIRE_FALSE(com->isEnabled());
    REQUIRE_FALSE(probe->isEnabled());

    bar.setVnaSettingsLocked(false);
    REQUIRE(backend->isEnabled());
    REQUIRE(host->isEnabled());
    REQUIRE(port->isEnabled());
    REQUIRE_FALSE(com->isEnabled());
    REQUIRE(probe->isEnabled());
}

TEST_CASE("plot wheel zooms and reset restores the full range", "[ui]")
{
    (void)application();
    S21PlotWidget plot;
    plot.resize(800, 500);
    plot.setCurves({4.9, 5.0, 5.1, 5.2, 5.3, 5.4, 5.5, 5.6, 5.7, 5.8, 5.9, 6.0},
                   {-30, -28, -20, -5, -1, -0.5, -0.4, -0.6, -1, -5, -20, -30},
                   {0, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110});

    QWheelEvent zoomIn(QPointF(400, 250), QPointF(400, 250), QPoint(), QPoint(0, 120),
                       Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(&plot, &zoomIn);
    REQUIRE(plot.visibleSpanFraction() == Catch::Approx(0.8));
    REQUIRE(plot.visibleStartFraction() > 0.0);

    const double beforePan = plot.visibleStartFraction();
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(400, 250), QPointF(400, 250),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent move(QEvent::MouseMove, QPointF(300, 250), QPointF(300, 250), Qt::NoButton,
                     Qt::LeftButton, Qt::NoModifier);
    QMouseEvent release(QEvent::MouseButtonRelease, QPointF(300, 250), QPointF(300, 250),
                        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(&plot, &press);
    QCoreApplication::sendEvent(&plot, &move);
    QCoreApplication::sendEvent(&plot, &release);
    REQUIRE(plot.visibleStartFraction() > beforePan);

    plot.resetView();
    REQUIRE(plot.visibleStartFraction() == Catch::Approx(0.0));
    REQUIRE(plot.visibleSpanFraction() == Catch::Approx(1.0));
}

TEST_CASE("plot overlays traces and requests multiple markers", "[ui]")
{
    (void)application();
    S21PlotWidget plot;
    plot.resize(800, 500);
    const QVector<double> frequency = {1.0, 1.1, 1.2};
    plot.setTraces({
        {QStringLiteral("S11"), frequency, {-10, -11, -12}, {0, 10, 20}, QColor(Qt::blue)},
        {QStringLiteral("S21"), frequency, {-1, -2, -3}, {30, 40, 50}, QColor(Qt::green)},
    });
    REQUIRE(plot.traceCount() == 2);

    QVector<double> requested;
    QObject::connect(&plot, &S21PlotWidget::markerRequested,
                     [&requested](double value) { requested.push_back(value); });
    plot.setMarkerPlacementEnabled(true);
    QMouseEvent first(QEvent::MouseButtonPress, QPointF(300, 100), QPointF(300, 100),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent second(QEvent::MouseButtonPress, QPointF(500, 100), QPointF(500, 100),
                       Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(&plot, &first);
    QCoreApplication::sendEvent(&plot, &second);
    REQUIRE(requested.size() == 2);
    REQUIRE(requested[0] < requested[1]);

    plot.setMarkerFrequencies({1.1});
    QImage rendered(plot.size(), QImage::Format_ARGB32_Premultiplied);
    rendered.fill(Qt::transparent);
    plot.render(&rendered);
    REQUIRE(rendered.pixelColor(10, 300) != QColor(Qt::green));
}

TEST_CASE("automatic plot markers calculate minimum maximum and average", "[ui]")
{
    const S21PlotTrace trace{
        QStringLiteral("S11"),
        {1.0, 1.1, 1.2},
        {-3.0, 1.0, 2.0},
        {},
        QColor(Qt::blue),
    };
    const auto statistics = plotTraceStatistics(trace);
    REQUIRE(statistics.valid);
    REQUIRE(statistics.minValue == Catch::Approx(-3.0));
    REQUIRE(statistics.minFrequencyGhz == Catch::Approx(1.0));
    REQUIRE(statistics.maxValue == Catch::Approx(2.0));
    REQUIRE(statistics.maxFrequencyGhz == Catch::Approx(1.2));
    REQUIRE(statistics.averageValue == Catch::Approx(0.0));
    REQUIRE(statistics.averageFrequencyGhz == Catch::Approx(1.075));
}

TEST_CASE("partial instrument trace does not erase other S-parameters", "[ui]")
{
    (void)application();
    MeasureTab tab;
    for (const auto* name : {"S11", "S21", "S12", "S22"}) {
        const auto* button = tab.findChild<QPushButton*>(
            QStringLiteral("graphTrace%1").arg(QString::fromLatin1(name)));
        REQUIRE(button != nullptr);
        REQUIRE(button->isChecked());
    }
    const QVector<double> oldAxis{1.0, 1.1, 1.2};
    const QVector<double> oldMag{-10.0, -11.0, -12.0};
    const QVector<double> oldPhase{0.0, 1.0, 2.0};
    tab.setSparamsCurves(oldAxis,
                         oldMag, oldPhase,
                         oldMag, oldPhase,
                         oldMag, oldPhase,
                         oldMag, oldPhase);
    auto* plot = tab.findChild<S21PlotWidget*>(QStringLiteral("graphPlot0"));
    REQUIRE(plot != nullptr);
    REQUIRE(plot->traceCount() == 4);

    const QVector<double> newAxis{2.0, 2.1, 2.2, 2.3};
    const QVector<double> newMag{-20.0, -21.0, -22.0, -23.0};
    const QVector<double> newPhase{3.0, 4.0, 5.0, 6.0};
    tab.setSparamsCurves(newAxis,
                         {}, {},
                         {}, {},
                         newMag, newPhase,
                         {}, {});

    auto* s11 = tab.findChild<QPushButton*>(QStringLiteral("graphTraceS11"));
    auto* s21 = tab.findChild<QPushButton*>(QStringLiteral("graphTraceS21"));
    auto* s12 = tab.findChild<QPushButton*>(QStringLiteral("graphTraceS12"));
    REQUIRE(s11 != nullptr);
    REQUIRE(s21 != nullptr);
    REQUIRE(s12 != nullptr);
    REQUIRE(plot != nullptr);

    s21->setChecked(false);
    s11->setChecked(true);
    REQUIRE(plot->primaryPointCount() == oldAxis.size());

    s11->setChecked(false);
    s12->setChecked(true);
    REQUIRE(plot->primaryPointCount() == newAxis.size());
}

TEST_CASE("two-port mode, center-span entry and report action are explicit", "[ui]")
{
    (void)application();
    MeasureTab tab;

    auto* workMode = tab.findChild<QComboBox*>(QStringLiteral("measurementWorkMode"));
    auto* stages = tab.findChild<QListWidget*>(QStringLiteral("measurementStages"));
    auto* stack = tab.findChild<QStackedWidget*>(QStringLiteral("measurementStageStack"));
    REQUIRE(workMode != nullptr);
    REQUIRE(stages != nullptr);
    REQUIRE(stack != nullptr);
    REQUIRE(workMode->currentIndex() == 0);
    for (const int stage : {2, 4, 5, 6}) {
        REQUIRE(stages->item(stage)->flags().testFlag(Qt::ItemIsEnabled));
        stages->setCurrentRow(stage);
        REQUIRE(stack->currentIndex() == stage);
    }
    workMode->setCurrentIndex(1);
    REQUIRE(stages->item(4)->flags().testFlag(Qt::ItemIsEnabled));

    tab.applyRunConfigDefaults(1.26e9, 1.35e9, 201, 1000, -20.0, 8);
    auto* entryMode = tab.findChild<QComboBox*>(QStringLiteral("frequencyEntryMode"));
    auto* center = tab.findChild<QDoubleSpinBox*>(QStringLiteral("frequencyCenter"));
    auto* span = tab.findChild<QDoubleSpinBox*>(QStringLiteral("frequencySpan"));
    REQUIRE(entryMode != nullptr);
    REQUIRE(center != nullptr);
    REQUIRE(span != nullptr);
    entryMode->setCurrentIndex(1);
    center->setValue(1300.0);
    span->setValue(90.0);
    REQUIRE(tab.fStartHz() == Catch::Approx(1.255e9));
    REQUIRE(tab.fStopHz() == Catch::Approx(1.345e9));

    auto* averages = tab.findChild<QSpinBox*>(QStringLiteral("sweepAverages"));
    REQUIRE(averages != nullptr);
    REQUIRE(averages->minimum() == 1);
    REQUIRE(averages->maximum() == 999);

    auto* calKit = tab.findChild<QSpinBox*>(QStringLiteral("vnaCalibrationKit"));
    REQUIRE(calKit != nullptr);
    REQUIRE(calKit->minimum() == 1);
    REQUIRE(calKit->maximum() == 64);

    auto* format = tab.findChild<QComboBox*>(QStringLiteral("graphDisplayFormat"));
    auto* exportButton = tab.findChild<QPushButton*>(QStringLiteral("exportTwoPortReport"));
    REQUIRE(format != nullptr);
    REQUIRE(format->count() >= 5);
    REQUIRE(exportButton != nullptr);
    REQUIRE_FALSE(exportButton->isEnabled());
    tab.setTwoPortExportEnabled(true);
    REQUIRE(exportButton->isEnabled());

    auto* deviceName = tab.findChild<QLineEdit*>(QStringLiteral("reportDeviceName"));
    auto* deviceSerial = tab.findChild<QLineEdit*>(QStringLiteral("reportDeviceSerial"));
    auto* operatorName = tab.findChild<QLineEdit*>(QStringLiteral("reportOperator"));
    auto* comment = tab.findChild<QLineEdit*>(QStringLiteral("reportComment"));
    auto* accepted = tab.findChild<QCheckBox*>(QStringLiteral("reportAccepted"));
    REQUIRE(deviceName != nullptr);
    REQUIRE(deviceSerial != nullptr);
    REQUIRE(operatorName != nullptr);
    REQUIRE(comment != nullptr);
    REQUIRE(accepted != nullptr);
    deviceName->setText(QStringLiteral("Изделие А"));
    deviceSerial->setText(QStringLiteral("SN-42"));
    operatorName->setText(QStringLiteral("Оператор"));
    comment->setText(QStringLiteral("Комментарий"));
    accepted->setChecked(true);
    REQUIRE(tab.reportDeviceName() == QStringLiteral("Изделие А"));
    REQUIRE(tab.reportDeviceSerial() == QStringLiteral("SN-42"));
    REQUIRE(tab.reportOperatorName() == QStringLiteral("Оператор"));
    REQUIRE(tab.reportComment() == QStringLiteral("Комментарий"));
    REQUIRE(tab.reportAccepted());
}

TEST_CASE("graph controls scroll after fullscreen is restored to a small window", "[ui]")
{
    auto& app = application();
    MeasureTab tab;
    tab.resize(1800, 1000);
    tab.show();
    app.processEvents();
    tab.resize(900, 520);
    app.processEvents();

    auto* controls = tab.findChild<QScrollArea*>(QStringLiteral("graphControlsScroll"));
    REQUIRE(controls != nullptr);
    REQUIRE(controls->width() <= 340);
    REQUIRE(controls->verticalScrollBar()->maximum() > 0);
}

TEST_CASE("one selected channel can use the complete 64 by 64 grid", "[ui]")
{
    (void)application();
    StartWizard wizard;
    auto* single = wizard.findChild<QRadioButton*>(QStringLiteral("seriesVolumeSingle"));
    auto* channel = wizard.findChild<QSpinBox*>(
        QStringLiteral("selectedCalibrationChannel"));
    REQUIRE(single != nullptr);
    REQUIRE(channel != nullptr);

    single->setChecked(true);
    channel->setValue(7);
    REQUIRE(wizard.singleChannelFullVolume());
    REQUIRE_FALSE(wizard.fullSeriesVolume());
    REQUIRE(channel->isEnabled());
    REQUIRE(wizard.selectedChannel() == 7);

    QTemporaryDir output;
    REQUIRE(output.isValid());
    auto* dataRoot = wizard.findChild<QLineEdit*>(QStringLiteral("seriesDataRoot"));
    REQUIRE(dataRoot != nullptr);
    dataRoot->setText(output.path());
    QString diagnostics;
    REQUIRE(wizard.materializeSimFixtures(diagnostics));

    QFile config(wizard.runConfigPath());
    REQUIRE(config.open(QIODevice::ReadOnly));
    const QByteArray json = config.readAll();
    REQUIRE(json.contains("\"channels\": { \"first\": 7, \"last\": 7 }"));
    REQUIRE(json.contains("\"phase_codes\": { \"first\": 0, \"last\": 63"));

    QFile attenuation(wizard.attenuatorCsvPath());
    REQUIRE(attenuation.open(QIODevice::ReadOnly));
    REQUIRE(attenuation.readAll().count('\n') == 65);
}

TEST_CASE("AT-12 telemetry creates a CSV sample immediately", "[ui]")
{
    auto& app = application();
    QTemporaryDir output;
    REQUIRE(output.isValid());

    {
        QSettings settings;
        settings.beginGroup(QStringLiteral("at_acceptance"));
        settings.clear();
        settings.setValue(QStringLiteral("at12/output_dir"), output.path());
        settings.endGroup();
        settings.sync();
    }

    AcceptanceAtTab tab;
    auto* start = tab.findChild<QPushButton*>(QStringLiteral("at12StartTelemetry"));
    auto* stop = tab.findChild<QPushButton*>(QStringLiteral("at12StopTelemetry"));
    auto* status = tab.findChild<QLabel*>(QStringLiteral("at12TelemetryStatus"));
    REQUIRE(start != nullptr);
    REQUIRE(stop != nullptr);
    REQUIRE(status != nullptr);
    REQUIRE(start->isEnabled());
    REQUIRE(QMetaObject::invokeMethod(&tab, "onAt12Start", Qt::DirectConnection));
    app.processEvents();

    QSettings result;
    result.beginGroup(QStringLiteral("at_acceptance"));
    const QString csvPath = result.value(QStringLiteral("at12/csv_path")).toString();
    const int sampleCount = result.value(QStringLiteral("at12/sample_count")).toInt();
    result.endGroup();
    INFO("CSV path: " << csvPath.toStdString());
    INFO("sample count: " << sampleCount);
    INFO("status: " << status->text().toStdString());
    REQUIRE_FALSE(csvPath.isEmpty());
    QFile csv(csvPath);
    REQUIRE(csv.open(QIODevice::ReadOnly | QIODevice::Text));
    const QByteArray contents = csv.readAll();
    REQUIRE(contents.startsWith("timestamp_utc,rss_bytes,private_bytes,handle_count,"));
    REQUIRE(contents.count('\n') == 2);
    REQUIRE(sampleCount == 1);

    REQUIRE(QMetaObject::invokeMethod(&tab, "onAt12Stop", Qt::DirectConnection));
    result.beginGroup(QStringLiteral("at_acceptance"));
    result.clear();
    result.endGroup();
    result.sync();
}
