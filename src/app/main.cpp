#include "ui/MainWindow.h"
#include "ui/Theme.h"

#include <QApplication>

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("AFAR RX Calibration Studio"));
    app.setOrganizationName(QStringLiteral("AFAR"));
    applyAppTheme(app, loadAppTheme());

    // Железо по умолчанию — VnaSimulator + DutSimulator внутри MeasureWorker
    // (не Socket/C2220Vna на боевой IP).
    MainWindow window;
    window.show();
    return app.exec();
}
