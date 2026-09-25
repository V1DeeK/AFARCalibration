#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "ConnectionBar.h"
#include "MeasureTab.h"
#include "S21PlotWidget.h"

#include <QApplication>
#include <QCoreApplication>
#include <QImage>
#include <QLabel>
#include <QMouseEvent>
#include <QPointF>
#include <QPushButton>
#include <QWheelEvent>

namespace {

QApplication& application()
{
    static int argc = 1;
    static char name[] = "test_ui_interactions";
    static char* argv[] = {name, nullptr};
    static QApplication app(argc, argv);
    return app;
}

}  // namespace

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
    REQUIRE(controller->text().contains(QStringLiteral("для S-параметров не нужен")));
    REQUIRE(controller->text().contains(QStringLiteral("CTRL:")));
    REQUIRE_FALSE(controller->text().contains(QStringLiteral("нет связи")));
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
    const QVector<double> oldAxis{1.0, 1.1, 1.2};
    const QVector<double> oldMag{-10.0, -11.0, -12.0};
    const QVector<double> oldPhase{0.0, 1.0, 2.0};
    tab.setSparamsCurves(oldAxis,
                         oldMag, oldPhase,
                         oldMag, oldPhase,
                         oldMag, oldPhase,
                         oldMag, oldPhase);

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
    auto* plot = tab.findChild<S21PlotWidget*>(QStringLiteral("graphPlot0"));
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
