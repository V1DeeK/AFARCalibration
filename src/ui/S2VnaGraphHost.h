#pragma once

#include <QWidget>

class QTimer;

/// Экспериментальный хост только для области графика запущенной S2VNA.
/// Не владеет процессом или окном S2VNA и возвращает TView на место при отключении.
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
    QTimer* m_watch = nullptr;
#ifdef Q_OS_WIN
    void* m_view = nullptr;
    void* m_originalParent = nullptr;
    void* m_host = nullptr;
    int m_originalX = 0;
    int m_originalY = 0;
    int m_originalWidth = 0;
    int m_originalHeight = 0;
#endif
};
