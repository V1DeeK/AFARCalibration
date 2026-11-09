#pragma once

#include <QWidget>

class QLabel;
class QTimer;

/// Живая копия всего окна S2VNA. Окно анализатора не переносится:
/// снимок PrintWindow кладётся на эту панель, S2VNA остаётся на месте.
class S2VnaGraphHost final : public QWidget {
    Q_OBJECT

public:
    explicit S2VnaGraphHost(QWidget* parent = nullptr);
    ~S2VnaGraphHost() override;

    [[nodiscard]] bool attach(QString* error);
    void detach();
    [[nodiscard]] bool isAttached() const;

signals:
    void windowLost();

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    void refreshMirror();

    QLabel* m_image = nullptr;
    QTimer* m_watch = nullptr;
#ifdef Q_OS_WIN
    void* m_source = nullptr;
#endif
};
