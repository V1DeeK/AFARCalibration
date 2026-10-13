#pragma once

#include "display.hpp"

#include <QElapsedTimer>
#include <QOpenGLWidget>

class QFocusEvent;
class QInputMethodEvent;
class QKeyEvent;
class QMouseEvent;
class QWheelEvent;

class SpectrumOpenGLWidget final : public QOpenGLWidget {
public:
    explicit SpectrumOpenGLWidget(AppState& state, QWidget* parent = nullptr);
    ~SpectrumOpenGLWidget() override;
    bool isRendererInitialized() const noexcept { return rendererInitialized_; }
    void setPage(SpectrumRenderer::Page page) { renderer_.setPage(page); update(); }

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
