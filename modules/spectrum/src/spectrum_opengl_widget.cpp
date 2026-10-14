#include "spectrum_opengl_widget.hpp"

#include "imgui.h"

#include <QFocusEvent>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QOpenGLContext>
#include <QSettings>
#include <QSurfaceFormat>
#include <QTimer>
#include <QWheelEvent>

#include <cfloat>

namespace {

ImGuiKey toImGuiKey(int key) {
    if (key >= Qt::Key_0 && key <= Qt::Key_9)
        return static_cast<ImGuiKey>(ImGuiKey_0 + key - Qt::Key_0);
    if (key >= Qt::Key_A && key <= Qt::Key_Z)
        return static_cast<ImGuiKey>(ImGuiKey_A + key - Qt::Key_A);
    if (key >= Qt::Key_F1 && key <= Qt::Key_F12)
        return static_cast<ImGuiKey>(ImGuiKey_F1 + key - Qt::Key_F1);

    switch (key) {
    case Qt::Key_Tab: return ImGuiKey_Tab;
    case Qt::Key_Left: return ImGuiKey_LeftArrow;
    case Qt::Key_Right: return ImGuiKey_RightArrow;
    case Qt::Key_Up: return ImGuiKey_UpArrow;
    case Qt::Key_Down: return ImGuiKey_DownArrow;
    case Qt::Key_PageUp: return ImGuiKey_PageUp;
    case Qt::Key_PageDown: return ImGuiKey_PageDown;
    case Qt::Key_Home: return ImGuiKey_Home;
    case Qt::Key_End: return ImGuiKey_End;
    case Qt::Key_Insert: return ImGuiKey_Insert;
    case Qt::Key_Delete: return ImGuiKey_Delete;
    case Qt::Key_Backspace: return ImGuiKey_Backspace;
    case Qt::Key_Space: return ImGuiKey_Space;
    case Qt::Key_Return: return ImGuiKey_Enter;
    case Qt::Key_Enter: return ImGuiKey_KeypadEnter;
    case Qt::Key_Escape: return ImGuiKey_Escape;
    case Qt::Key_Apostrophe: return ImGuiKey_Apostrophe;
    case Qt::Key_Comma: return ImGuiKey_Comma;
    case Qt::Key_Minus: return ImGuiKey_Minus;
    case Qt::Key_Period: return ImGuiKey_Period;
    case Qt::Key_Slash: return ImGuiKey_Slash;
    case Qt::Key_Semicolon: return ImGuiKey_Semicolon;
    case Qt::Key_Equal: return ImGuiKey_Equal;
    case Qt::Key_BracketLeft: return ImGuiKey_LeftBracket;
    case Qt::Key_Backslash: return ImGuiKey_Backslash;
    case Qt::Key_BracketRight: return ImGuiKey_RightBracket;
    case Qt::Key_QuoteLeft: return ImGuiKey_GraveAccent;
    default: return ImGuiKey_None;
    }
}

int mouseButtonIndex(Qt::MouseButton button) {
    switch (button) {
    case Qt::LeftButton: return 0;
    case Qt::RightButton: return 1;
    case Qt::MiddleButton: return 2;
    case Qt::BackButton: return 3;
    case Qt::ForwardButton: return 4;
    default: return -1;
    }
}

void submitModifiers(ImGuiIO& io, Qt::KeyboardModifiers modifiers) {
    io.AddKeyEvent(ImGuiMod_Ctrl, modifiers.testFlag(Qt::ControlModifier));
    io.AddKeyEvent(ImGuiMod_Shift, modifiers.testFlag(Qt::ShiftModifier));
    io.AddKeyEvent(ImGuiMod_Alt, modifiers.testFlag(Qt::AltModifier));
    io.AddKeyEvent(ImGuiMod_Super, modifiers.testFlag(Qt::MetaModifier));
}

} // namespace

QSurfaceFormat SpectrumOpenGLWidget::surfaceFormat() {
    QSurfaceFormat format;
    format.setRenderableType(QSurfaceFormat::OpenGL);
    format.setVersion(3, 3);
    format.setProfile(QSurfaceFormat::CoreProfile);
    format.setSwapBehavior(QSurfaceFormat::DoubleBuffer);
    format.setSwapInterval(1);
    return format;
}

SpectrumOpenGLWidget::SpectrumOpenGLWidget(AppState& state, QWidget* parent)
    : QOpenGLWidget(parent), state_(state), renderer_(state) {
    setFormat(surfaceFormat());

    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setAttribute(Qt::WA_InputMethodEnabled, true);
    loadSettings();
    renderer_.syncTextFieldsFromState();

    auto* timer = new QTimer(this);
    timer->setInterval(16);
    connect(timer, &QTimer::timeout, this, [this] {
        if (isVisible())
            update();
    });
    timer->start();
}

SpectrumOpenGLWidget::~SpectrumOpenGLWidget() {
    saveSettings();
    if (context() && context()->isValid()) {
        makeCurrent();
        renderer_.shutdown();
        doneCurrent();
    }
}

void SpectrumOpenGLWidget::loadSettings() {
    QSettings settings;
    settings.beginGroup(QStringLiteral("spectrum"));

    {
        std::lock_guard<std::mutex> lock(state_.connection_mutex);
        state_.pluto_ip = settings.value(QStringLiteral("device_uri"),
                                         QString::fromStdString(state_.pluto_ip)).toString().toStdString();
    }
    state_.sample_rate_hz.store(settings.value(QStringLiteral("sample_rate_hz"),
                                                state_.sample_rate_hz.load()).toUInt());
    state_.bandwidth_hz.store(settings.value(QStringLiteral("bandwidth_hz"),
                                              state_.bandwidth_hz.load()).toUInt());
    state_.center_freq_hz.store(settings.value(QStringLiteral("center_freq_hz"),
                                                static_cast<qulonglong>(state_.center_freq_hz.load())).toULongLong());
    state_.fft_size.store(settings.value(QStringLiteral("fft_size"), state_.fft_size.load()).toInt());
    state_.rf_port_idx.store(settings.value(QStringLiteral("rf_port"), state_.rf_port_idx.load()).toInt());
    state_.chan_mode.store(static_cast<PlutoChannelMode>(qBound(
        0, settings.value(QStringLiteral("channel_mode"),
                          static_cast<int>(state_.chan_mode.load())).toInt(), 2)));
    state_.gain_mode.store(static_cast<GainMode>(qBound(
        0, settings.value(QStringLiteral("gain_mode"),
                          static_cast<int>(state_.gain_mode.load())).toInt(), 3)));
    state_.gain_db.store(settings.value(QStringLiteral("gain_db"), state_.gain_db.load()).toInt());
    state_.gain_db2.store(settings.value(QStringLiteral("gain_db2"), state_.gain_db2.load()).toInt());
    state_.gain_link_channels.store(settings.value(QStringLiteral("gain_link_channels"),
                                                    state_.gain_link_channels.load()).toBool());
    state_.phase_stable_mode.store(settings.value(QStringLiteral("phase_stable"),
                                                   state_.phase_stable_mode.load()).toBool());
    state_.rx_phase_inversion.store(settings.value(QStringLiteral("phase_inversion"),
                                                    state_.rx_phase_inversion.load()).toBool());
    state_.rx_cal_enabled.store(settings.value(QStringLiteral("calibration_enabled"),
                                                state_.rx_cal_enabled.load()).toBool());
    state_.exclude_dc_bin.store(settings.value(QStringLiteral("exclude_dc"),
                                                state_.exclude_dc_bin.load()).toBool());
    state_.cal_offset_db.store(settings.value(QStringLiteral("cal_offset_db"),
                                               state_.cal_offset_db.load()).toFloat());
    state_.demod_mode.store(static_cast<DemodMode>(qBound(
        0, settings.value(QStringLiteral("demod_mode"),
                          static_cast<int>(state_.demod_mode.load())).toInt(), 1)));
    state_.audio_volume.store(settings.value(QStringLiteral("audio_volume"),
                                              state_.audio_volume.load()).toFloat());
    state_.audio_mute.store(settings.value(QStringLiteral("audio_mute"),
                                            state_.audio_mute.load()).toBool());
    state_.playback_loop.store(settings.value(QStringLiteral("playback_loop"),
                                               state_.playback_loop.load()).toBool());
    state_.playback_dual_channel.store(settings.value(QStringLiteral("playback_dual_channel"),
                                                       state_.playback_dual_channel.load()).toBool());
    state_.peleng_use_network.store(settings.value(QStringLiteral("peleng_use_network"),
                                                    state_.peleng_use_network.load()).toBool());
    state_.peleng_net_port.store(static_cast<uint16_t>(qBound(
        1, settings.value(QStringLiteral("peleng_port"), state_.peleng_net_port.load()).toInt(), 65535)));

    {
        std::lock_guard<std::mutex> lock(state_.rx_cal_mutex);
        state_.rx_cal_path = settings.value(QStringLiteral("calibration_path"),
                                             QString::fromStdString(state_.rx_cal_path)).toString().toStdString();
    }
    {
        std::lock_guard<std::mutex> lock(state_.playback_mutex);
        state_.playback_file_path = settings.value(QStringLiteral("playback_path"),
                                                    QString::fromStdString(state_.playback_file_path)).toString().toStdString();
    }
    renderer_.setPage(static_cast<SpectrumRenderer::Page>(qBound(
        0, settings.value(QStringLiteral("active_page"), 0).toInt(), 2)));
    renderer_.setLanguage(settings.value(QStringLiteral("language"), QStringLiteral("ru")).toString()
                                  == QStringLiteral("en")
                              ? SpectrumRenderer::Language::English
                              : SpectrumRenderer::Language::Russian);
    settings.endGroup();
}

void SpectrumOpenGLWidget::saveSettings() const {
    QSettings settings;
    settings.beginGroup(QStringLiteral("spectrum"));
    {
        std::lock_guard<std::mutex> lock(state_.connection_mutex);
        settings.setValue(QStringLiteral("device_uri"), QString::fromStdString(state_.pluto_ip));
    }
    settings.setValue(QStringLiteral("sample_rate_hz"), state_.sample_rate_hz.load());
    settings.setValue(QStringLiteral("bandwidth_hz"), state_.bandwidth_hz.load());
    settings.setValue(QStringLiteral("center_freq_hz"), QVariant::fromValue(state_.center_freq_hz.load()));
    settings.setValue(QStringLiteral("fft_size"), state_.fft_size.load());
    settings.setValue(QStringLiteral("rf_port"), state_.rf_port_idx.load());
    settings.setValue(QStringLiteral("channel_mode"), static_cast<int>(state_.chan_mode.load()));
    settings.setValue(QStringLiteral("gain_mode"), static_cast<int>(state_.gain_mode.load()));
    settings.setValue(QStringLiteral("gain_db"), state_.gain_db.load());
    settings.setValue(QStringLiteral("gain_db2"), state_.gain_db2.load());
    settings.setValue(QStringLiteral("gain_link_channels"), state_.gain_link_channels.load());
    settings.setValue(QStringLiteral("phase_stable"), state_.phase_stable_mode.load());
    settings.setValue(QStringLiteral("phase_inversion"), state_.rx_phase_inversion.load());
    settings.setValue(QStringLiteral("calibration_enabled"), state_.rx_cal_enabled.load());
    settings.setValue(QStringLiteral("exclude_dc"), state_.exclude_dc_bin.load());
    settings.setValue(QStringLiteral("cal_offset_db"), state_.cal_offset_db.load());
    settings.setValue(QStringLiteral("demod_mode"), static_cast<int>(state_.demod_mode.load()));
    settings.setValue(QStringLiteral("audio_volume"), state_.audio_volume.load());
    settings.setValue(QStringLiteral("audio_mute"), state_.audio_mute.load());
    settings.setValue(QStringLiteral("playback_loop"), state_.playback_loop.load());
    settings.setValue(QStringLiteral("playback_dual_channel"), state_.playback_dual_channel.load());
    settings.setValue(QStringLiteral("peleng_use_network"), state_.peleng_use_network.load());
    settings.setValue(QStringLiteral("peleng_port"), state_.peleng_net_port.load());
    settings.setValue(QStringLiteral("active_page"), static_cast<int>(renderer_.page()));
    settings.setValue(QStringLiteral("language"),
                      renderer_.language() == SpectrumRenderer::Language::English
                          ? QStringLiteral("en") : QStringLiteral("ru"));
    {
        std::lock_guard<std::mutex> lock(state_.rx_cal_mutex);
        settings.setValue(QStringLiteral("calibration_path"), QString::fromStdString(state_.rx_cal_path));
    }
    {
        std::lock_guard<std::mutex> lock(state_.playback_mutex);
        settings.setValue(QStringLiteral("playback_path"), QString::fromStdString(state_.playback_file_path));
    }
    settings.endGroup();
}

void SpectrumOpenGLWidget::initializeGL() {
    rendererInitialized_ = renderer_.initialize();
    frameTimer_.start();
}

void SpectrumOpenGLWidget::paintGL() {
    const float deltaSeconds = frameTimer_.isValid()
        ? static_cast<float>(frameTimer_.restart()) / 1000.0f
        : 1.0f / 60.0f;
    renderer_.renderFrame(static_cast<float>(width()), static_cast<float>(height()),
                          static_cast<float>(devicePixelRatioF()), deltaSeconds);
}

void SpectrumOpenGLWidget::mouseMoveEvent(QMouseEvent* event) {
    if (renderer_.imguiContext()) {
        ImGui::SetCurrentContext(renderer_.imguiContext());
        ImGui::GetIO().AddMousePosEvent(static_cast<float>(event->position().x()),
                                        static_cast<float>(event->position().y()));
    }
    event->accept();
}

void SpectrumOpenGLWidget::mousePressEvent(QMouseEvent* event) {
    if (renderer_.imguiContext()) {
        ImGui::SetCurrentContext(renderer_.imguiContext());
        const int button = mouseButtonIndex(event->button());
        if (button >= 0)
            ImGui::GetIO().AddMouseButtonEvent(button, true);
        setFocus(Qt::MouseFocusReason);
    }
    event->accept();
}

void SpectrumOpenGLWidget::mouseReleaseEvent(QMouseEvent* event) {
    if (renderer_.imguiContext()) {
        ImGui::SetCurrentContext(renderer_.imguiContext());
        const int button = mouseButtonIndex(event->button());
        if (button >= 0)
            ImGui::GetIO().AddMouseButtonEvent(button, false);
    }
    event->accept();
}

void SpectrumOpenGLWidget::wheelEvent(QWheelEvent* event) {
    if (renderer_.imguiContext()) {
        ImGui::SetCurrentContext(renderer_.imguiContext());
        const QPoint delta = event->angleDelta();
        ImGui::GetIO().AddMouseWheelEvent(static_cast<float>(delta.x()) / 120.0f,
                                          static_cast<float>(delta.y()) / 120.0f);
    }
    event->accept();
}

void SpectrumOpenGLWidget::keyPressEvent(QKeyEvent* event) {
    submitKeyEvent(event, true);
    event->accept();
}

void SpectrumOpenGLWidget::keyReleaseEvent(QKeyEvent* event) {
    submitKeyEvent(event, false);
    event->accept();
}

void SpectrumOpenGLWidget::submitKeyEvent(QKeyEvent* event, bool pressed) {
    if (!renderer_.imguiContext())
        return;

    ImGui::SetCurrentContext(renderer_.imguiContext());
    ImGuiIO& io = ImGui::GetIO();
    submitModifiers(io, event->modifiers());
    const ImGuiKey key = toImGuiKey(event->key());
    if (key != ImGuiKey_None)
        io.AddKeyEvent(key, pressed);

    if (pressed && !event->text().isEmpty()
        && !event->modifiers().testFlag(Qt::ControlModifier)) {
        const QByteArray utf8 = event->text().toUtf8();
        io.AddInputCharactersUTF8(utf8.constData());
    }
}

void SpectrumOpenGLWidget::inputMethodEvent(QInputMethodEvent* event) {
    if (renderer_.imguiContext() && !event->commitString().isEmpty()) {
        ImGui::SetCurrentContext(renderer_.imguiContext());
        const QByteArray utf8 = event->commitString().toUtf8();
        ImGui::GetIO().AddInputCharactersUTF8(utf8.constData());
    }
    event->accept();
}

void SpectrumOpenGLWidget::focusInEvent(QFocusEvent* event) {
    if (renderer_.imguiContext()) {
        ImGui::SetCurrentContext(renderer_.imguiContext());
        ImGui::GetIO().AddFocusEvent(true);
    }
    QOpenGLWidget::focusInEvent(event);
}

void SpectrumOpenGLWidget::focusOutEvent(QFocusEvent* event) {
    if (renderer_.imguiContext()) {
        ImGui::SetCurrentContext(renderer_.imguiContext());
        ImGui::GetIO().AddFocusEvent(false);
    }
    QOpenGLWidget::focusOutEvent(event);
}

void SpectrumOpenGLWidget::leaveEvent(QEvent* event) {
    if (renderer_.imguiContext()) {
        ImGui::SetCurrentContext(renderer_.imguiContext());
        ImGui::GetIO().AddMousePosEvent(-FLT_MAX, -FLT_MAX);
    }
    QOpenGLWidget::leaveEvent(event);
}
