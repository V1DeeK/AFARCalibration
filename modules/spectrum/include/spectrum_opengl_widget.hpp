#pragma once

#include "display.hpp"

#include <QElapsedTimer>
#include <QOpenGLWidget>
#include <QSurfaceFormat>

class QFocusEvent;
class QInputMethodEvent;
class QKeyEvent;
class QMouseEvent;
class QWheelEvent;

class SpectrumOpenGLWidget final : public QOpenGLWidget {
public:
    /// Формат поверхности OpenGL 3.3 Core. Его нужно задать как формат по умолчанию
    /// (QSurfaceFormat::setDefaultFormat) ДО создания QApplication: на macOS, если
    /// формат окна и виджета различаются, составной кадр QOpenGLWidget не
    /// отображается (чёрная вкладка с перевёрнутым содержимым окна).
    static QSurfaceFormat surfaceFormat();

    explicit SpectrumOpenGLWidget(AppState& state, QWidget* parent = nullptr);
    ~SpectrumOpenGLWidget() override;
    bool isRendererInitialized() const noexcept { return rendererInitialized_; }
    void setPage(SpectrumRenderer::Page page) { renderer_.setPage(page); update(); }
    /// Светлая или тёмная тема графиков; применяется на следующем кадре.
    void setLightTheme(bool light) { renderer_.setLightTheme(light); update(); }
    [[nodiscard]] bool lightTheme() const noexcept { return renderer_.lightTheme(); }

protected:
    void initializeGL() override;
    void paintGL() override;

    void mouseMoveEvent(QMouseEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void inputMethodEvent(QInputMethodEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    void loadSettings();
    void saveSettings() const;
    void submitKeyEvent(QKeyEvent* event, bool pressed);

    AppState& state_;
    SpectrumRenderer renderer_;
    QElapsedTimer frameTimer_;
    bool rendererInitialized_{ false };
};
