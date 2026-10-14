#include "app_state.hpp"
#include "spectrum_opengl_widget.hpp"

#include <QApplication>
#include <QTimer>

int main(int argc, char* argv[]) {
    QSurfaceFormat::setDefaultFormat(SpectrumOpenGLWidget::surfaceFormat());
    QApplication app(argc, argv);
    app.setOrganizationName(QStringLiteral("AFAR"));
    app.setApplicationName(QStringLiteral("SpectrumWidgetTest"));
    AppState state;
    state.running.store(false);

    SpectrumOpenGLWidget widget(state);
    widget.resize(900, 700);
    widget.show();

    int result = 1;
    int page = 0;
    QTimer pageTimer;
    pageTimer.setInterval(120);
    QObject::connect(&pageTimer, &QTimer::timeout, &app, [&] {
        widget.setPage(static_cast<SpectrumRenderer::Page>(page));
        ++page;
        if (page < 3)
            return;
        pageTimer.stop();
        QTimer::singleShot(120, &app, [&] {
            result = widget.isRendererInitialized() ? 0 : 1;
            app.quit();
        });
    });
    pageTimer.start();
    app.exec();
    return result;
}
