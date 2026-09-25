#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "ConnectionBar.h"
#include "S21PlotWidget.h"

#include <QApplication>
#include <QCoreApplication>
#include <QLabel>
#include <QMouseEvent>
#include <QPointF>
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
