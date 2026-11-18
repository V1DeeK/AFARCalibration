#include "VideoReceiverTab.h"
#include "Theme.h"

#include "UiStyle.h"
#include "heter/HeterSession.h"
#include "video/VideoSession.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QScrollArea>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTableWidget>
#include <QThread>
#include <QVBoxLayout>

#ifdef AFAR_HAVE_MULTIMEDIA
#include <QCamera>
#include <QImageCapture>
#include <QMediaCaptureSession>
#include <QMediaDevices>
#include <QMediaRecorder>
#include <QUrl>
#include <QVideoWidget>
#endif

namespace {

QString settingKey(const char* name)
{
    return QStringLiteral("videoReceiver/") + QLatin1String(name);
}

QString stamp()
{
    return QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd_HH-mm-ss"));
}

QString documentsDir()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)
                        + QStringLiteral("/AFAR RX Calibration/video-receiver");
    QDir().mkpath(dir);
    return dir;
}

QTableWidget* makeTable(const QStringList& headers, QWidget* parent)
{
    auto* table = new QTableWidget(0, static_cast<int>(headers.size()), parent);
    table->setHorizontalHeaderLabels(headers);
    table->horizontalHeader()->setStretchLastSection(true);
    table->verticalHeader()->setVisible(false);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    return table;
}

void addRow(QTableWidget* table, const QStringList& cells, const QColor& tint = {}, int tintColumn = -1)
{
    const int row = table->rowCount();
    table->insertRow(row);
    for (int i = 0; i < cells.size(); ++i) {
        auto* item = new QTableWidgetItem(cells.at(i));
        if (i == tintColumn && tint.isValid()) {
            item->setForeground(tint);
        }
        table->setItem(row, i, item);
    }
    table->scrollToBottom();
}

QColor resultColor(const QString& result)
{
    if (result == QLatin1String("PASS") || result == QStringLiteral("ГОДНА")) {
        return ui::okColor();
    }
    if (result == QLatin1String("WARN")) {
        return ui::warnColor();
    }
    if (result == QLatin1String("SKIP")) {
        return {};
    }
    return ui::errorColor();
}

}  // namespace

/// RSSI и ослабление за последние 60 с.
class RssiChart final : public QWidget {
public:
    explicit RssiChart(QWidget* parent)
        : QWidget(parent)
    {
        setMinimumHeight(140);
        m_clock.start();
    }

    void add(int rssiMv, double attenDb)
    {
        m_points.append({m_clock.elapsed(), rssiMv, attenDb});
        while (!m_points.isEmpty() && m_clock.elapsed() - m_points.first().t > 60000) {
            m_points.removeFirst();
        }
        update();
    }

    void setTarget(int mv)
    {
        m_target = mv;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter g(this);
        g.setRenderHint(QPainter::Antialiasing);
        const double w = width();
        const double h = height();
        // Весь диапазон АЦП: на плате №1 RSSI 1,9…3,1 В.
        const auto y = [h](double mv) { return h - 6 - (mv / 3300.0) * (h - 12); };
        QColor zone = themeColor(ThemeColor::Ok);
        zone.setAlpha(36);
        g.fillRect(QRectF(0, y(m_target + 50), w, y(m_target - 50) - y(m_target + 50)), zone);
        for (const int mv : {1000, 2000, 3000}) {
            g.setPen(themeColor(ThemeColor::Border));
            g.drawLine(QPointF(0, y(mv)), QPointF(w, y(mv)));
            g.setPen(themeColor(ThemeColor::Muted));
            g.drawText(QPointF(4, y(mv) - 3), QStringLiteral("%1 мВ").arg(mv));
        }
        if (m_points.size() < 2) {
            return;
        }
        const qint64 now = m_clock.elapsed();
        const auto x = [w, now](qint64 t) { return w - (now - t) / 60000.0 * w; };
        QPolygonF rssi;
        QPolygonF atten;
        for (const Point& p : m_points) {
            rssi << QPointF(x(p.t), y(p.rssi));
            atten << QPointF(x(p.t), h - 6 - (p.atten / 31.5) * (h - 12));
        }
        g.setPen(QPen(themeColor(ThemeColor::Muted), 1.5));
        g.drawPolyline(atten);
        g.setPen(QPen(seriesColor(0), 2));
        g.drawPolyline(rssi);
    }

private:
    struct Point {
        qint64 t;
        int rssi;
        double atten;
    };
    QList<Point> m_points;
    QElapsedTimer m_clock;
    int m_target = 850;
};

/// Видео с выхода CVBS (J5) через USB-карту видеозахвата. Без Qt Multimedia — только пояснение.
class VideoCapturePane final : public QWidget {
public:
    explicit VideoCapturePane(QWidget* parent)
        : QWidget(parent)
    {
        auto* lay = new QVBoxLayout(this);
        lay->setContentsMargins(0, 0, 0, 0);
#ifdef AFAR_HAVE_MULTIMEDIA
        auto* row = new QHBoxLayout();
        m_devices = new QComboBox(this);
        m_devices->setMinimumWidth(260);
        auto* find = new QPushButton(QStringLiteral("Найти"), this);
        auto* show = new QPushButton(QStringLiteral("Показать"), this);
        ui::setButtonRole(show, ui::ButtonRole::Primary);
        auto* stop = new QPushButton(QStringLiteral("Стоп"), this);
        auto* shot = new QPushButton(QStringLiteral("Снимок PNG"), this);
        m_record = new QPushButton(QStringLiteral("● Запись"), this);
        m_info = ui::hintLabel(QString(), this);
        row->addWidget(new QLabel(QStringLiteral("Источник"), this));
        row->addWidget(m_devices, 1);
        for (QPushButton* b : {find, show, stop, shot, m_record}) {
            row->addWidget(b);
        }
        lay->addLayout(row);
        m_view = new QVideoWidget(this);
        m_view->setMinimumHeight(300);
        lay->addWidget(m_view, 1);
        lay->addWidget(m_info);
        m_capture.setVideoOutput(m_view);
        m_capture.setImageCapture(&m_image);
        m_capture.setRecorder(&m_recorder);
        connect(find, &QPushButton::clicked, this, [this] { findDevices(); });
        connect(show, &QPushButton::clicked, this, [this] { start(); });
        connect(stop, &QPushButton::clicked, this, [this] { stop_(); });
        connect(shot, &QPushButton::clicked, this, [this] {
            if (m_camera == nullptr) {
                return;
            }
            const QString path = documentsDir() + QStringLiteral("/kadr_") + stamp() + QStringLiteral(".png");
            m_image.captureToFile(path);
            m_info->setText(QStringLiteral("снимок: ") + path);
        });
        connect(m_record, &QPushButton::clicked, this, [this] {
            if (m_camera == nullptr) {
                return;
            }
            if (m_recorder.recorderState() == QMediaRecorder::RecordingState) {
                m_recorder.stop();
                m_record->setText(QStringLiteral("● Запись"));
                m_info->setText(QStringLiteral("запись: ") + m_recorder.actualLocation().toLocalFile());
                return;
            }
            m_recorder.setOutputLocation(QUrl::fromLocalFile(documentsDir() + QStringLiteral("/video_") + stamp()));
            m_recorder.record();
            m_record->setText(QStringLiteral("■ Остановить запись"));
        });
        connect(&m_recorder, &QMediaRecorder::errorOccurred, this,
                [this](QMediaRecorder::Error, const QString& text) { m_info->setText(QStringLiteral("запись: ") + text); });
        m_info->setText(QStringLiteral("Подключите выход CVBS платы (J5: 1 или 3 — сигнал, 2 — GND) к USB-карте "
                                       "видеозахвата, нажмите «Найти», выберите карту и «Показать»."));
#else
        lay->addWidget(ui::hintLabel(QStringLiteral("Сборка без Qt Multimedia: видео с карты захвата недоступно. "
                                                    "Установите модуль qt6-multimedia и пересоберите программу."),
                                     this));
#endif
    }

private:
#ifdef AFAR_HAVE_MULTIMEDIA
    void findDevices()
    {
        m_devices->clear();
        m_found = QMediaDevices::videoInputs();
        static const QStringList hints{QStringLiteral("grab"), QStringLiteral("easycap"), QStringLiteral("capture"),
                                       QStringLiteral("av to usb"), QStringLiteral("usb video"),
                                       QStringLiteral("macrosilicon"), QStringLiteral("uvc"), QStringLiteral("ms2109")};
        for (const QCameraDevice& d : m_found) {
            m_devices->addItem(d.description());
            const QString name = d.description().toLower();
            for (const QString& hint : hints) {
                if (name.contains(hint)) {
                    m_devices->setCurrentIndex(m_devices->count() - 1);
                }
            }
        }
        m_info->setText(m_found.isEmpty() ? QStringLiteral("карта захвата не найдена — проверьте подключение")
                                          : QStringLiteral("найдено устройств: %1").arg(m_found.size()));
    }

    void start()
    {
        stop_();
        const int index = m_devices->currentIndex();
        if (index < 0 || index >= m_found.size()) {
            m_info->setText(QStringLiteral("сначала «Найти» и выберите карту"));
            return;
        }
        m_camera = new QCamera(m_found.at(index), this);
        connect(m_camera, &QCamera::errorOccurred, this,
                [this](QCamera::Error, const QString& text) { m_info->setText(QStringLiteral("не удалось открыть: ") + text); });
        m_capture.setCamera(m_camera);
        m_camera->start();
        m_info->setText(m_found.at(index).description());
    }

    void stop_()
    {
        if (m_recorder.recorderState() == QMediaRecorder::RecordingState) {
            m_recorder.stop();
            m_record->setText(QStringLiteral("● Запись"));
        }
        if (m_camera != nullptr) {
            m_camera->stop();
            m_capture.setCamera(nullptr);
            m_camera->deleteLater();
            m_camera = nullptr;
        }
    }

    QComboBox* m_devices = nullptr;
    QPushButton* m_record = nullptr;
    QLabel* m_info = nullptr;
    QVideoWidget* m_view = nullptr;
    QList<QCameraDevice> m_found;
    QCamera* m_camera = nullptr;
    QMediaCaptureSession m_capture;
    QImageCapture m_image;
    QMediaRecorder m_recorder;
#endif
};

VideoReceiverTab::VideoReceiverTab(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("videoReceiverTab"));
    m_thread = new QThread(this);
    m_thread->setObjectName(QStringLiteral("video-receiver"));
    m_session = new VideoSession();
    m_session->moveToThread(m_thread);
    connect(m_thread, &QThread::finished, m_session, &QObject::deleteLater);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(ui::kPageMargin, ui::kPageMargin, ui::kPageMargin, ui::kPageMargin);
    root->setSpacing(ui::kGap);
    root->addWidget(buildConnection());
    auto* body = new QHBoxLayout();
    body->setSpacing(ui::kGap);
    m_pages = new QTabWidget(this);
    m_pages->setObjectName(QStringLiteral("videoReceiverPages"));
    m_pages->addTab(buildMonitor(), QStringLiteral("Монитор"));
    {
        auto* page = new QWidget(m_pages);
        auto* lay = new QVBoxLayout(page);
        auto* box = new QGroupBox(QStringLiteral("Видео с выхода CVBS (J5 → USB-карта видеозахвата)"), page);
        auto* boxLay = new QVBoxLayout(box);
        ui::applyGroupLayout(boxLay);
        m_capture = new VideoCapturePane(box);
        boxLay->addWidget(m_capture, 1);
        auto* scanRow = new QHBoxLayout();
        scanRow->addWidget(new QLabel(QStringLiteral("Быстрая перестройка"), box));
        const int race[] = {5658, 5695, 5732, 5769, 5806, 5843, 5880, 5917};
        for (int i = 0; i < 8; ++i) {
            auto* b = new QPushButton(QStringLiteral("R%1 %2").arg(i + 1).arg(race[i]), box);
            const int mhz = race[i];
            connect(b, &QPushButton::clicked, this, [this, mhz] { applyNow(mhz); });
            scanRow->addWidget(b);
        }
        m_scan = new QPushButton(QStringLiteral("Обзор диапазона"), box);
        scanRow->addWidget(m_scan);
        boxLay->addLayout(scanRow);
        m_scanInfo = ui::hintLabel(QStringLiteral("«Обзор диапазона» перебирает каналы Raceband, A, B, E, F при "
                                                  "ослаблении 0 дБ и показывает RSSI каждого."), box);
        boxLay->addWidget(m_scanInfo);
        m_scanTable = makeTable({QStringLiteral("Канал"), QStringLiteral("МГц"), QStringLiteral("RSSI, мВ")}, box);
        m_scanTable->setMaximumHeight(180);
        m_scanTable->setSortingEnabled(false);
        boxLay->addWidget(m_scanTable);
        lay->addWidget(box);
        connect(m_scan, &QPushButton::clicked, this, [this] {
            if (!m_open || m_busy) {
                return;
            }
            m_scanTable->setRowCount(0);
            setBusy(true);
            const video::ApplyRequest a = requestFromUi(m_freq->value());
            QMetaObject::invokeMethod(m_session, [s = m_session, a] { s->scanChannels(a.agcMode, a.attenCode, a.targetMv, a.power); },
                                      Qt::QueuedConnection);
        });
        connect(m_scanTable, &QTableWidget::cellDoubleClicked, this, [this](int row, int) {
            if (auto* item = m_scanTable->item(row, 1)) {
                applyNow(item->text().toDouble());
            }
        });
        m_pages->addTab(page, QStringLiteral("Видео"));
    }
    m_pages->addTab(buildTest(), QStringLiteral("Проверка платы"));
    m_pages->addTab(buildFirmware(), QStringLiteral("Прошивка"));
    m_pages->addTab(buildJournal(), QStringLiteral("Журнал"));
    body->addWidget(m_pages, 3);
    body->addWidget(buildStatus(), 1);
    root->addLayout(body, 1);

    connect(m_session, &VideoSession::statusUpdated, this, &VideoReceiverTab::showStatus);
    connect(m_session, &VideoSession::opDone, this, &VideoReceiverTab::showOp);
    connect(m_session, &VideoSession::linkChanged, this, [this](const QString& kind, const QString& text) {
        m_link->setText(text);
        ui::setTone(m_link, kind == QLatin1String("on") ? ui::Tone::Ok
                            : kind == QLatin1String("boot") ? ui::Tone::Warn
                            : kind == QLatin1String("err") ? ui::Tone::Error : ui::Tone::Neutral);
        const bool open = kind != QLatin1String("off") && !(kind == QLatin1String("err") && !m_open);
        if (kind == QLatin1String("off") || text == QStringLiteral("ошибка порта")
            || text == QStringLiteral("сборка без Qt SerialPort")) {
            m_open = false;
        } else if (open) {
            m_open = true;
        }
        if (text == QStringLiteral("нет ответа")) {
            m_state->setText(QStringLiteral("нет связи"));
        }
        m_connect->setText(m_open ? QStringLiteral("Отключить") : QStringLiteral("Подключить"));
        updateControls();
    });
    connect(m_session, &VideoSession::logLine, this, [this](const QString& kind, const QString& text) {
        const QString prefix = kind == QLatin1String("er") ? QStringLiteral("! ") : QString();
        m_log->appendPlainText(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz  ")) + prefix + text);
    });
    connect(m_session, &VideoSession::statsChanged, this, [this](int ok, int errors) {
        m_stats->setText(QStringLiteral("ok %1 · ош %2").arg(ok).arg(errors));
    });
    connect(m_session, &VideoSession::registersRead, this, [this](const QList<quint32>& words) {
        m_regs->setRowCount(0);
        for (int i = 0; i < words.size(); ++i) {
            addRow(m_regs, {video::regTitle(i),
                            QStringLiteral("0x") + QString::number(words.at(i), 16).toUpper().rightJustified(8, QLatin1Char('0')),
                            video::regMeaning(i, words.at(i))});
        }
        m_regs->scrollToTop();
    });
    connect(m_session, &VideoSession::testStep, this, [this](const QString& name, const QString& result, const QString& text) {
        addRow(m_testTable, {name, result, text}, resultColor(result), 1);
        m_current.rows.append({name, result, text});
    });
    connect(m_session, &VideoSession::testFinished, this,
            [this](const QString& verdict, int fails, int warns, const video::Status& identity) {
                m_current.verdict = verdict;
                m_current.identity = identity;
                m_verdict->setText(QStringLiteral("%1 — %2 ошибок, %3 предупреждений").arg(verdict).arg(fails).arg(warns));
                ui::setTone(m_verdict, verdict == QStringLiteral("ГОДНА") ? ui::Tone::Ok : ui::Tone::Error);
                m_history.prepend(m_current);
                m_histTable->insertRow(0);
                const QStringList cells{m_current.time, m_current.serial.isEmpty() ? QStringLiteral("—") : m_current.serial,
                                        identity.bootText(), identity.fwText(), verdict};
                for (int i = 0; i < cells.size(); ++i) {
                    auto* item = new QTableWidgetItem(cells.at(i));
                    if (i == 4) {
                        item->setForeground(resultColor(verdict));
                    }
                    m_histTable->setItem(0, i, item);
                }
                m_reportSave->setEnabled(true);
                setBusy(false);
            });
    connect(m_session, &VideoSession::scanRow, this, [this](const QString& name, int mhz, int rssi) {
        addRow(m_scanTable, {name, QString::number(mhz), QString::number(rssi)});
    });
    connect(m_session, &VideoSession::scanFinished, this, [this](int bestMhz, const QString& text) {
        m_scanInfo->setText(text + QStringLiteral(" Двойной щелчок по строке — настроиться на канал."));
        m_scanTable->sortItems(2, Qt::DescendingOrder);
        if (bestMhz > 0) {
            m_freq->setValue(bestMhz);
        }
        setBusy(false);
    });
    connect(m_session, &VideoSession::flashProgress, this, [this](double fraction, const QString& text) {
        m_fwBar->setValue(static_cast<int>(fraction * 1000));
        if (!text.isEmpty()) {
            m_fwStatus->setText(text);
            ui::setTone(m_fwStatus, ui::Tone::Info);
        }
    });
    connect(m_session, &VideoSession::flashFinished, this, [this](bool ok, const QString& text) {
        m_fwStatus->setText(text);
        ui::setTone(m_fwStatus, ok ? ui::Tone::Ok : ui::Tone::Error);
        setBusy(false);
    });

    m_thread->start();
    refreshPorts();
    updateControls();
}

VideoReceiverTab::~VideoReceiverTab()
{
    m_session->abort();
    QMetaObject::invokeMethod(m_session, &VideoSession::closePort, Qt::BlockingQueuedConnection);
    m_thread->quit();
    m_thread->wait();
}

QWidget* VideoReceiverTab::buildConnection()
{
    auto* box = new QGroupBox(QStringLiteral("Модуль видеоприёма Э.468116.00133 — узел VIDEO, RS-485 (Modbus RTU 115200 8E1)"), this);
    auto* lay = new QHBoxLayout(box);
    ui::applyGroupLayout(lay);
    m_port = new QComboBox(box);
    m_port->setObjectName(QStringLiteral("videoPort"));
    m_port->setMinimumWidth(240);
    auto* refresh = new QPushButton(QStringLiteral("Обновить"), box);
    m_addr = new QSpinBox(box);
    m_addr->setRange(1, 247);
    m_addr->setValue(3);
    m_connect = new QPushButton(QStringLiteral("Подключить"), box);
    m_connect->setObjectName(QStringLiteral("videoConnect"));
    ui::setButtonRole(m_connect, ui::ButtonRole::Primary);
    m_link = new QLabel(QStringLiteral("не подключено"), box);
    m_link->setObjectName(QStringLiteral("videoLink"));
    lay->addWidget(new QLabel(QStringLiteral("Порт USB-RS485"), box));
    lay->addWidget(m_port);
    lay->addWidget(refresh);
    lay->addWidget(new QLabel(QStringLiteral("Адрес"), box));
    lay->addWidget(m_addr);
    lay->addWidget(m_connect);
    lay->addWidget(m_link);
    lay->addStretch(1);
    connect(refresh, &QPushButton::clicked, this, &VideoReceiverTab::refreshPorts);
    connect(m_connect, &QPushButton::clicked, this, &VideoReceiverTab::toggleConnection);
    if (!VideoSession::serialSupported()) {
        m_connect->setEnabled(false);
        m_link->setText(QStringLiteral("сборка без Qt SerialPort"));
    }
    return box;
}

QWidget* VideoReceiverTab::buildMonitor()
{
    auto* page = new QWidget(this);
    auto* lay = new QVBoxLayout(page);
    lay->setSpacing(ui::kGap);

    auto* rx = new QGroupBox(QStringLiteral("Настройка приёма"), page);
    auto* grid = new QGridLayout(rx);
    ui::applyGroupLayout(grid);
    m_freq = new QDoubleSpinBox(rx);
    m_freq->setObjectName(QStringLiteral("videoFreq"));
    m_freq->setRange(4000.0, 6500.0);
    m_freq->setDecimals(3);
    m_freq->setSuffix(QStringLiteral(" МГц"));
    m_freq->setValue(5800.0);
    m_preset = new QComboBox(rx);
    m_preset->addItem(QStringLiteral("— каналы —"));
    for (const int f : {5658, 5695, 5732, 5769, 5806, 5843, 5880, 5917, 4900, 5200, 5500, 6000}) {
        m_preset->addItem(QString::number(f), f);
    }
    connect(m_preset, &QComboBox::activated, this, [this](int index) {
        if (index > 0) {
            m_freq->setValue(m_preset->itemData(index).toDouble());
        }
        m_preset->setCurrentIndex(0);
    });
    m_profile = new QComboBox(rx);
    m_profile->addItem(QStringLiteral("1 — частота сигнала (LO = F − 480 МГц)"), 1);
    m_profile->addItem(QStringLiteral("2 — частота LO (4420…5520 МГц)"), 2);
    m_rfEnable = new QCheckBox(QStringLiteral("Гетеродин включён"), rx);
    m_rfEnable->setChecked(true);
    m_power = new QComboBox(rx);
    for (const char* p : {"−4 дБм", "−1 дБм", "+2 дБм", "+5 дБм"}) {
        m_power->addItem(QString::fromUtf8(p));
    }
    m_power->setCurrentIndex(3);
    m_agc = new QComboBox(rx);
    m_agc->addItem(QStringLiteral("авто"), 0);
    m_agc->addItem(QStringLiteral("ручной"), 1);
    m_agc->setCurrentIndex(1);
    m_attenDb = new QDoubleSpinBox(rx);
    m_attenDb->setRange(0.0, 31.5);
    m_attenDb->setSingleStep(0.5);
    m_attenDb->setDecimals(1);
    m_attenDb->setSuffix(QStringLiteral(" дБ"));
    m_target = new QSpinBox(rx);
    m_target->setRange(500, 3300);
    m_target->setSingleStep(10);
    m_target->setValue(2500);
    m_target->setSuffix(QStringLiteral(" мВ"));
    m_sendExt = new QCheckBox(QStringLiteral("передавать VIDEO-EXT"), rx);
    m_sendExt->setChecked(true);
    m_bandMode = new QComboBox(rx);
    m_bandMode->addItem(QStringLiteral("авто по частоте"), 0);
    m_bandMode->addItem(QStringLiteral("низкий: HFCN-4600 (4900–5500)"), 1);
    m_bandMode->addItem(QStringLiteral("высокий: HFCN-5500 (5500–6000)"), 2);
    m_cp = new QComboBox(rx);
    for (int c = 0; c <= 15; ++c) {
        m_cp->addItem(QStringLiteral("%1 — %2 мА ном.").arg(c).arg((c + 1) * 0.3, 0, 'f', 1), c);
    }
    m_cp->setCurrentIndex(2);
    m_cpWrite = new QPushButton(QStringLiteral("Записать"), rx);
    m_cpResult = ui::hintLabel(QString(), rx);
    m_apply = new QPushButton(QStringLiteral("Применить"), rx);
    m_apply->setObjectName(QStringLiteral("videoApply"));
    ui::setButtonRole(m_apply, ui::ButtonRole::Primary);
    m_safe = new QPushButton(QStringLiteral("SAFE"), rx);
    ui::setButtonRole(m_safe, ui::ButtonRole::Danger);
    m_clear = new QPushButton(QStringLiteral("Сброс FAULT"), rx);
    m_applyResult = ui::hintLabel(QString(), rx);
    m_rtcAudio = new QCheckBox(QStringLiteral("аудио 6,0/6,5 МГц"), rx);
    m_rtcRf = new QCheckBox(QStringLiteral("собственный ВЧ (синтезатор, смеситель)"), rx);
    m_rtcWrite = new QPushButton(QStringLiteral("Записать"), rx);
    m_rtcResult = ui::hintLabel(QString(), rx);

    int r = 0;
    grid->addWidget(new QLabel(QStringLiteral("Частота"), rx), r, 0);
    grid->addWidget(m_freq, r, 1);
    grid->addWidget(m_preset, r, 2);
    grid->addWidget(new QLabel(QStringLiteral("Профиль"), rx), r, 3);
    grid->addWidget(m_profile, r++, 4, 1, 2);
    grid->addWidget(m_rfEnable, r, 0, 1, 2);
    grid->addWidget(new QLabel(QStringLiteral("Мощность LO"), rx), r, 3);
    grid->addWidget(m_power, r++, 4);
    grid->addWidget(new QLabel(QStringLiteral("АРУ"), rx), r, 0);
    grid->addWidget(m_agc, r, 1);
    grid->addWidget(new QLabel(QStringLiteral("Ослабление"), rx), r, 2);
    grid->addWidget(m_attenDb, r, 3);
    grid->addWidget(new QLabel(QStringLiteral("Уставка RSSI"), rx), r, 4);
    grid->addWidget(m_target, r++, 5);
    grid->addWidget(new QLabel(QStringLiteral("Фильтр преселектора"), rx), r, 0);
    grid->addWidget(m_bandMode, r, 1, 1, 2);
    grid->addWidget(m_sendExt, r++, 3, 1, 2);
    grid->addWidget(new QLabel(QStringLiteral("Ток накачки ADF4355-3"), rx), r, 0);
    grid->addWidget(m_cp, r, 1);
    grid->addWidget(m_cpWrite, r, 2);
    grid->addWidget(m_cpResult, r++, 3, 1, 3);
    auto* buttons = new QHBoxLayout();
    buttons->addWidget(m_apply);
    buttons->addWidget(m_safe);
    buttons->addWidget(m_clear);
    buttons->addWidget(m_applyResult, 1);
    grid->addLayout(buttons, r++, 0, 1, 6);
    auto* rtc = new QHBoxLayout();
    rtc->addWidget(new QLabel(QStringLiteral("Тракты RTC6715"), rx));
    rtc->addWidget(m_rtcAudio);
    rtc->addWidget(m_rtcRf);
    rtc->addWidget(m_rtcWrite);
    rtc->addWidget(m_rtcResult, 1);
    grid->addLayout(rtc, r++, 0, 1, 6);
    grid->addWidget(ui::hintLabel(QStringLiteral(
                        "Фильтр выбирается вместе с «Применить» (нужна галочка VIDEO-EXT). Принудительный выбор — для "
                        "проверки веток HMC8038 и подавления зеркального канала (зеркало = F − 960 МГц). Ток накачки и "
                        "тракты RTC6715 (регистр 0410) применяются сразу, без COMMIT."), rx),
                    r, 0, 1, 6);
    lay->addWidget(rx);

    connect(m_apply, &QPushButton::clicked, this, [this] { applyNow(m_freq->value()); });
    connect(m_target, &QSpinBox::valueChanged, this, [this](int mv) { m_chart->setTarget(mv); });
    const auto simple = [this](QPushButton* button, QLabel* result, const std::function<void()>& call) {
        connect(button, &QPushButton::clicked, this, [this, result, call] {
            if (!m_open || m_busy) {
                return;
            }
            result->setText(QStringLiteral("…"));
            call();
        });
    };
    simple(m_safe, m_applyResult, [this] { QMetaObject::invokeMethod(m_session, &VideoSession::safe, Qt::QueuedConnection); });
    simple(m_clear, m_applyResult, [this] { QMetaObject::invokeMethod(m_session, &VideoSession::clearFault, Qt::QueuedConnection); });
    simple(m_cpWrite, m_cpResult, [this] {
        const int code = m_cp->currentData().toInt();
        QMetaObject::invokeMethod(m_session, [s = m_session, code] { s->writeChargePump(code); }, Qt::QueuedConnection);
    });
    simple(m_rtcWrite, m_rtcResult, [this] {
        const int v = (m_rtcAudio->isChecked() ? 1 : 0) | (m_rtcRf->isChecked() ? 2 : 0);
        QMetaObject::invokeMethod(m_session, [s = m_session, v] { s->writeRtcPaths(v); }, Qt::QueuedConnection);
    });

    auto* regs = new QGroupBox(QStringLiteral("Регистры ADF4355-3 — как записаны в гетеродин"), page);
    auto* regsLay = new QVBoxLayout(regs);
    ui::applyGroupLayout(regsLay);
    auto* regsRow = new QHBoxLayout();
    m_regsRead = new QPushButton(QStringLiteral("Прочитать"), regs);
    m_regsInfo = ui::hintLabel(QString(), regs);
    regsRow->addWidget(m_regsRead);
    regsRow->addWidget(m_regsInfo, 1);
    regsLay->addLayout(regsRow);
    m_regs = makeTable({QStringLiteral("Регистр"), QStringLiteral("Слово"), QStringLiteral("Назначение")}, regs);
    m_regs->setObjectName(QStringLiteral("videoRegs"));
    m_regs->setMinimumHeight(200);
    m_regs->setColumnWidth(0, 260);
    regsLay->addWidget(m_regs);
    lay->addWidget(regs);
    simple(m_regsRead, m_regsInfo, [this] { QMetaObject::invokeMethod(m_session, &VideoSession::readRegisters, Qt::QueuedConnection); });

    auto* osd = new QGroupBox(QStringLiteral("OSD — AT7456E, 30 столбцов"), page);
    auto* osdLay = new QVBoxLayout(osd);
    ui::applyGroupLayout(osdLay);
    auto* o1 = new QHBoxLayout();
    m_osdStd = new QComboBox(osd);
    m_osdStd->addItem(QStringLiteral("PAL (16 строк)"), 1);
    m_osdStd->addItem(QStringLiteral("NTSC (13 строк)"), 0);
    m_osdRow = new QSpinBox(osd);
    m_osdRow->setRange(0, 15);
    m_osdRow->setValue(1);
    m_osdCol = new QSpinBox(osd);
    m_osdCol->setRange(0, 29);
    m_osdCol->setValue(1);
    m_osdEnable = new QCheckBox(QStringLiteral("OSD вкл."), osd);
    m_osdEnable->setChecked(true);
    o1->addWidget(new QLabel(QStringLiteral("Стандарт"), osd));
    o1->addWidget(m_osdStd);
    o1->addWidget(new QLabel(QStringLiteral("Строка"), osd));
    o1->addWidget(m_osdRow);
    o1->addWidget(new QLabel(QStringLiteral("Столбец"), osd));
    o1->addWidget(m_osdCol);
    o1->addWidget(m_osdEnable);
    o1->addStretch(1);
    osdLay->addLayout(o1);
    auto* o2 = new QHBoxLayout();
    m_osdText = new QLineEdit(QStringLiteral("VIDEO RX 5800"), osd);
    m_osdText->setMaxLength(30);
    m_osdFont = new QComboBox(osd);
    m_osdFont->addItem(QStringLiteral("заводской MAX7456"), false);
    m_osdFont->addItem(QStringLiteral("ASCII 1:1"), true);
    m_osdInv = new QCheckBox(QStringLiteral("инверсия"), osd);
    m_osdBlink = new QCheckBox(QStringLiteral("мигание"), osd);
    m_osdBg = new QCheckBox(QStringLiteral("фон"), osd);
    o2->addWidget(new QLabel(QStringLiteral("Текст"), osd));
    o2->addWidget(m_osdText, 1);
    o2->addWidget(m_osdFont);
    o2->addWidget(m_osdInv);
    o2->addWidget(m_osdBlink);
    o2->addWidget(m_osdBg);
    osdLay->addLayout(o2);
    auto* o3 = new QHBoxLayout();
    m_osdWrite = new QPushButton(QStringLiteral("Записать в OSD"), osd);
    m_osdClear = new QPushButton(QStringLiteral("Очистить строку"), osd);
    m_osdResult = ui::hintLabel(QString(), osd);
    o3->addWidget(m_osdWrite);
    o3->addWidget(m_osdClear);
    o3->addWidget(m_osdResult, 1);
    osdLay->addLayout(o3);
    m_osdCodes = ui::hintLabel(QString(), osd);
    osdLay->addWidget(m_osdCodes);
    lay->addWidget(osd);
    const auto codes = [this] {
        QStringList hex;
        for (const quint16 g : video::glyphsFor(m_osdText->text(), m_osdFont->currentData().toBool())) {
            hex << QString::number(g, 16).rightJustified(2, QLatin1Char('0'));
        }
        m_osdCodes->setText(QStringLiteral("Коды символов: ") + hex.join(QLatin1Char(' ')));
    };
    connect(m_osdText, &QLineEdit::textChanged, this, codes);
    connect(m_osdFont, &QComboBox::currentIndexChanged, this, codes);
    codes();
    const auto sendOsd = [this](int col, const QList<quint16>& glyphs) {
        const int std = m_osdStd->currentData().toInt();
        const bool enable = m_osdEnable->isChecked();
        const int row = m_osdRow->value();
        const int attr = (m_osdInv->isChecked() ? 1 : 0) | (m_osdBlink->isChecked() ? 2 : 0) | (m_osdBg->isChecked() ? 4 : 0);
        QMetaObject::invokeMethod(m_session, [s = m_session, std, enable, row, col, glyphs, attr] {
            s->osdWrite(std, enable, row, col, glyphs, attr);
        }, Qt::QueuedConnection);
    };
    simple(m_osdWrite, m_osdResult, [this, sendOsd] {
        const int col = m_osdCol->value();
        sendOsd(col, video::glyphsFor(m_osdText->text(), m_osdFont->currentData().toBool()).mid(0, 30 - col));
    });
    simple(m_osdClear, m_osdResult, [this, sendOsd] {
        sendOsd(0, QList<quint16>(30, m_osdFont->currentData().toBool() ? 0x20 : 0x00));
    });

    auto* chart = new QGroupBox(QStringLiteral("RSSI и ослабление — последние 60 с"), page);
    auto* chartLay = new QVBoxLayout(chart);
    ui::applyGroupLayout(chartLay);
    m_chart = new RssiChart(chart);
    m_chart->setTarget(m_target->value());
    chartLay->addWidget(m_chart);
    chartLay->addWidget(ui::hintLabel(QStringLiteral("Синяя линия — RSSI, мВ (зелёная полоса — уставка АРУ ± 50 мВ). "
                                                     "Серая — ослабление HMC624A, дБ."), chart));
    lay->addWidget(chart);
    lay->addStretch(1);
    return ui::scrollPage(page, this);
}

QWidget* VideoReceiverTab::buildTest()
{
    auto* page = new QWidget(this);
    auto* lay = new QVBoxLayout(page);
    auto* box = new QGroupBox(QStringLiteral("Автоматическая проверка"), page);
    auto* boxLay = new QVBoxLayout(box);
    ui::applyGroupLayout(boxLay);
    auto* r1 = new QHBoxLayout();
    m_serial = new QLineEdit(box);
    m_serial->setPlaceholderText(QStringLiteral("например 049-0012"));
    m_operator = new QLineEdit(box);
    r1->addWidget(new QLabel(QStringLiteral("Зав. номер"), box));
    r1->addWidget(m_serial);
    r1->addWidget(new QLabel(QStringLiteral("Проверяющий"), box));
    r1->addWidget(m_operator);
    boxLay->addLayout(r1);
    auto* r2 = new QHBoxLayout();
    m_haveSignal = new QCheckBox(QStringLiteral("на вход подан тестовый сигнал"), box);
    m_sigFreq = new QDoubleSpinBox(box);
    m_sigFreq->setRange(4900.0, 6000.0);
    m_sigFreq->setDecimals(0);
    m_sigFreq->setValue(5800.0);
    m_sigFreq->setSuffix(QStringLiteral(" МГц"));
    m_haveVideo = new QCheckBox(QStringLiteral("ожидается видео на выходе"), box);
    r2->addWidget(m_haveSignal);
    r2->addWidget(m_sigFreq);
    r2->addWidget(m_haveVideo);
    r2->addStretch(1);
    boxLay->addLayout(r2);
    auto* r3 = new QHBoxLayout();
    m_testStart = new QPushButton(QStringLiteral("Запустить проверку"), box);
    m_testStart->setObjectName(QStringLiteral("videoTestStart"));
    ui::setButtonRole(m_testStart, ui::ButtonRole::Primary);
    m_testStop = new QPushButton(QStringLiteral("Остановить"), box);
    ui::setButtonRole(m_testStop, ui::ButtonRole::Danger);
    m_reportSave = new QPushButton(QStringLiteral("Сохранить протокол"), box);
    m_reportSave->setEnabled(false);
    m_verdict = new QLabel(box);
    r3->addWidget(m_testStart);
    r3->addWidget(m_testStop);
    auto* reportFolder = new QPushButton(QStringLiteral("Папка протокола"), box);
    r3->addWidget(m_reportSave);
    r3->addWidget(reportFolder);
    connect(reportFolder, &QPushButton::clicked, this, [] {
        const QString path = QSettings().value(QStringLiteral("video/report_path")).toString();
        const QString dir = path.isEmpty() ? QString() : QFileInfo(path).absolutePath();
        if (!dir.isEmpty() && QDir(dir).exists()) {
            QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
        }
    });
    r3->addWidget(m_verdict, 1);
    boxLay->addLayout(r3);
    boxLay->addWidget(ui::hintLabel(QStringLiteral(
        "Проверка занимает ~15 с. Без тестового сигнала проверяются связь, синтезатор (захват PLL на краях диапазона), "
        "аттенюатор, OSD, SAFE и сторожевой таймер. С сигналом — дополнительно реакция RSSI на ослабление и АРУ."), box));
    lay->addWidget(box);
    m_testTable = makeTable({QStringLiteral("Проверка"), QStringLiteral("Результат"), QStringLiteral("Подробности")}, page);
    m_testTable->setObjectName(QStringLiteral("videoTestTable"));
    m_testTable->setColumnWidth(0, 300);
    m_testTable->setMinimumHeight(260);
    lay->addWidget(m_testTable, 2);
    auto* hist = new QGroupBox(QStringLiteral("Проверенные платы за сеанс"), page);
    auto* histLay = new QVBoxLayout(hist);
    ui::applyGroupLayout(histLay);
    m_histTable = makeTable({QStringLiteral("Время"), QStringLiteral("Зав. №"), QStringLiteral("BOOT_ID"),
                             QStringLiteral("Версия ПО"), QStringLiteral("Итог")}, hist);
    m_histTable->setMaximumHeight(140);
    histLay->addWidget(m_histTable);
    auto* histButtons = new QHBoxLayout();
    auto* csv = new QPushButton(QStringLiteral("Сохранить CSV"), hist);
    auto* csvFolder = new QPushButton(QStringLiteral("Папка CSV"), hist);
    histButtons->addWidget(csv);
    histButtons->addWidget(csvFolder);
    histButtons->addStretch(1);
    histLay->addLayout(histButtons);
    connect(csvFolder, &QPushButton::clicked, this, [] {
        const QString path = QSettings().value(QStringLiteral("video/history_path")).toString();
        const QString dir = path.isEmpty() ? QString() : QFileInfo(path).absolutePath();
        if (!dir.isEmpty() && QDir(dir).exists()) {
            QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
        }
    });
    lay->addWidget(hist, 1);

    connect(m_testStart, &QPushButton::clicked, this, [this] {
        if (!m_open || m_busy) {
            return;
        }
        m_testTable->setRowCount(0);
        m_verdict->clear();
        m_reportSave->setEnabled(false);
        m_current = {};
        m_current.time = QDateTime::currentDateTime().toString(QStringLiteral("dd.MM.yyyy HH:mm:ss"));
        m_current.serial = m_serial->text().trimmed();
        m_current.operatorName = m_operator->text().trimmed();
        m_current.signal = m_haveSignal->isChecked() ? QStringLiteral("%1 МГц").arg(m_sigFreq->value(), 0, 'f', 0)
                                                     : QStringLiteral("нет");
        video::TestOptions opt;
        opt.serial = m_current.serial;
        opt.haveSignal = m_haveSignal->isChecked();
        opt.signalMhz = m_sigFreq->value();
        opt.haveVideo = m_haveVideo->isChecked();
        opt.asciiFont = m_osdFont->currentData().toBool();
        setBusy(true);
        QMetaObject::invokeMethod(m_session, [s = m_session, opt] { s->runTest(opt); }, Qt::QueuedConnection);
    });
    connect(m_testStop, &QPushButton::clicked, this, [this] { m_session->abort(); });
    connect(m_reportSave, &QPushButton::clicked, this, &VideoReceiverTab::saveReport);
    connect(csv, &QPushButton::clicked, this, &VideoReceiverTab::saveHistory);
    return page;
}

QWidget* VideoReceiverTab::buildFirmware()
{
    auto* page = new QWidget(this);
    auto* lay = new QVBoxLayout(page);
    auto* box = new QGroupBox(QStringLiteral("Обновление прошивки по RS-485"), page);
    auto* boxLay = new QVBoxLayout(box);
    ui::applyGroupLayout(boxLay);
    auto* r1 = new QHBoxLayout();
    m_fwPath = new QLineEdit(box);
    m_fwPath->setReadOnly(true);
    auto* choose = new QPushButton(QStringLiteral("Выбрать файл…"), box);
    m_fwVersion = new QLineEdit(QStringLiteral("0.1"), box);
    m_fwVersion->setMaximumWidth(70);
    r1->addWidget(new QLabel(QStringLiteral("Файл"), box));
    auto* fwFolder = new QPushButton(QStringLiteral("Папка"), box);
    r1->addWidget(m_fwPath, 1);
    r1->addWidget(fwFolder);
    r1->addWidget(choose);
    connect(fwFolder, &QPushButton::clicked, this, [this] {
        const QString path = m_fwPath->text().trimmed();
        const QString dir = path.isEmpty() ? QString() : QFileInfo(path).absolutePath();
        if (!dir.isEmpty() && QDir(dir).exists()) {
            QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
        }
    });
    r1->addWidget(new QLabel(QStringLiteral("Версия"), box));
    r1->addWidget(m_fwVersion);
    boxLay->addLayout(r1);
    m_fwInfo = ui::hintLabel(QString(), box);
    boxLay->addWidget(m_fwInfo);
    auto* r2 = new QHBoxLayout();
    m_flash = new QPushButton(QStringLiteral("Прошить"), box);
    m_flash->setObjectName(QStringLiteral("videoFlash"));
    ui::setButtonRole(m_flash, ui::ButtonRole::Primary);
    m_recover = new QPushButton(QStringLiteral("Восстановление…"), box);
    m_fwStop = new QPushButton(QStringLiteral("Остановить"), box);
    ui::setButtonRole(m_fwStop, ui::ButtonRole::Danger);
    r2->addWidget(m_flash);
    r2->addWidget(m_recover);
    r2->addWidget(m_fwStop);
    r2->addStretch(1);
    boxLay->addLayout(r2);
    m_fwBar = new QProgressBar(box);
    m_fwBar->setRange(0, 1000);
    boxLay->addWidget(m_fwBar);
    m_fwStatus = new QLabel(box);
    m_fwStatus->setWordWrap(true);
    boxLay->addWidget(m_fwStatus);
    boxLay->addWidget(ui::hintLabel(QStringLiteral(
        "Нужен файл video_receiver.bin из сборки (приложение с адреса 0x08002000), а на плате — загрузчик, прошитый один "
        "раз через ST-Link (video_receiver_full.hex). Во время прошивки остановите опрос шины другими мастерами. Если "
        "обновление прервалось, плата останется в загрузчике и прошивку можно повторить.\n"
        "Восстановление — если приложение не отвечает: нажмите кнопку и в течение 10 с выключите и включите питание "
        "платы. Загрузчик слушает шину 0,5 с после включения."), box));
    lay->addWidget(box);
    lay->addStretch(1);
    connect(choose, &QPushButton::clicked, this, &VideoReceiverTab::chooseFirmware);
    connect(m_flash, &QPushButton::clicked, this, [this] {
        if (!m_open || m_busy || m_image.isEmpty()) {
            return;
        }
        const QStringList parts = m_fwVersion->text().split(QLatin1Char('.'));
        const quint32 version = (static_cast<quint32>(parts.value(0).toUInt() & 0xFFFF) << 16)
                                | (parts.value(1).toUInt() & 0xFFFF);
        m_fwBar->setValue(0);
        setBusy(true);
        QMetaObject::invokeMethod(m_session, [s = m_session, image = m_image, version] { s->flash(image, version); },
                                  Qt::QueuedConnection);
    });
    connect(m_recover, &QPushButton::clicked, this, [this] {
        if (!m_open || m_busy) {
            return;
        }
        setBusy(true);
        QMetaObject::invokeMethod(m_session, &VideoSession::recover, Qt::QueuedConnection);
    });
    connect(m_fwStop, &QPushButton::clicked, this, [this] { m_session->abort(); });
    return page;
}

QWidget* VideoReceiverTab::buildJournal()
{
    auto* page = new QWidget(this);
    auto* lay = new QVBoxLayout(page);
    auto* row = new QHBoxLayout();
    m_logPoll = new QCheckBox(QStringLiteral("показывать опрос статуса"), page);
    auto* clear = new QPushButton(QStringLiteral("Очистить"), page);
    row->addWidget(m_logPoll);
    row->addWidget(clear);
    row->addStretch(1);
    lay->addLayout(row);
    m_log = new QPlainTextEdit(page);
    m_log->setObjectName(QStringLiteral("videoLog"));
    m_log->setReadOnly(true);
    m_log->setMaximumBlockCount(1500);
    QFont mono(QStringLiteral("Menlo"));
    mono.setStyleHint(QFont::Monospace);
    m_log->setFont(mono);
    lay->addWidget(m_log, 1);
    connect(m_logPoll, &QCheckBox::toggled, this, [this](bool on) { m_session->setLogPoll(on); });
    connect(clear, &QPushButton::clicked, m_log, &QPlainTextEdit::clear);
    return page;
}

QWidget* VideoReceiverTab::buildStatus()
{
    auto* box = new QGroupBox(QStringLiteral("Состояние узла"), this);
    box->setMinimumWidth(290);
    auto* lay = new QVBoxLayout(box);
    ui::applyGroupLayout(lay);
    m_state = new QLabel(QStringLiteral("—"), box);
    m_state->setObjectName(QStringLiteral("videoState"));
    QFont big = m_state->font();
    big.setPointSizeF(big.pointSizeF() * 1.4);
    big.setBold(true);
    m_state->setFont(big);
    m_error = ui::hintLabel(QString(), box);
    m_flags = new QLabel(box);
    m_flags->setWordWrap(true);
    lay->addWidget(m_state);
    lay->addWidget(m_error);
    lay->addWidget(m_flags);
    auto* form = new QFormLayout();
    const QStringList names{QStringLiteral("Режим"), QStringLiteral("ПО / загрузчик"), QStringLiteral("Частота (прим.)"),
                            QStringLiteral("Расчётная"), QStringLiteral("Фильтр преселектора"),
                            QStringLiteral("Гетеродин (LO)"), QStringLiteral("RSSI"), QStringLiteral("Ослабление"),
                            QStringLiteral("АРУ"), QStringLiteral("Видео (STAT)"), QStringLiteral("RTC6715 аудио / ВЧ"),
                            QStringLiteral("BOOT_ID"), QStringLiteral("Время работы"),
                            QStringLiteral("ACCEPTED / APPLIED"), QStringLiteral("LOCAL_EPOCH")};
    for (const QString& name : names) {
        auto* value = new QLabel(QStringLiteral("—"), box);
        value->setWordWrap(true);
        value->setTextInteractionFlags(Qt::TextSelectableByMouse);
        form->addRow(ui::captionLabel(name, box), value);
        m_values.append(value);
    }
    lay->addLayout(form);
    m_autoPoll = new QCheckBox(QStringLiteral("опрос 300 мс"), box);
    m_autoPoll->setChecked(true);
    connect(m_autoPoll, &QCheckBox::toggled, this, [this](bool on) { m_session->setPollEnabled(on); });
    m_stats = ui::hintLabel(QString(), box);
    lay->addWidget(m_autoPoll);
    lay->addWidget(m_stats);
    lay->addWidget(ui::hintLabel(QStringLiteral("Без запросов более 1 с узел сам уходит в SAFE и выключает гетеродин "
                                                "(требование PF60-HW) — держите опрос включённым."), box));
    lay->addStretch(1);
    return box;
}

void VideoReceiverTab::refreshPorts()
{
    const QString current = m_port->currentData().toString();
    m_port->clear();
    for (const HeterPortChoice& choice : HeterSession::ports()) {
        m_port->addItem(choice.label, choice.name);
    }
    QSettings s;
    const QString wanted = current.isEmpty() ? s.value(settingKey("port")).toString() : current;
    const int index = m_port->findData(wanted);
    if (index >= 0) {
        m_port->setCurrentIndex(index);
    }
    if (m_port->count() == 0) {
        m_port->addItem(QStringLiteral("адаптеров USB-RS485 не найдено"), QString());
    }
}

void VideoReceiverTab::toggleConnection()
{
    if (m_open) {
        m_session->abort();
        QMetaObject::invokeMethod(m_session, &VideoSession::closePort, Qt::QueuedConnection);
        return;
    }
    const QString port = m_port->currentData().toString();
    if (port.isEmpty()) {
        m_link->setText(QStringLiteral("выберите порт"));
        ui::setTone(m_link, ui::Tone::Warn);
        return;
    }
    QSettings s;
    s.setValue(settingKey("port"), port);
    const int addr = m_addr->value();
    QMetaObject::invokeMethod(m_session, [session = m_session, port, addr] { session->openPort(port, addr); },
                              Qt::QueuedConnection);
}

void VideoReceiverTab::setBusy(bool busy)
{
    m_busy = busy;
    updateControls();
}

void VideoReceiverTab::updateControls()
{
    const bool ready = m_open && !m_busy;
    for (QPushButton* b : {m_apply, m_safe, m_clear, m_cpWrite, m_rtcWrite, m_regsRead, m_osdWrite, m_osdClear,
                           m_testStart, m_recover, m_scan}) {
        b->setEnabled(ready);
    }
    m_flash->setEnabled(ready && !m_image.isEmpty());
    m_testStop->setEnabled(m_busy);
    m_fwStop->setEnabled(m_busy);
    m_port->setEnabled(!m_open);
    m_addr->setEnabled(!m_open);
}

video::ApplyRequest VideoReceiverTab::requestFromUi(double mhz) const
{
    video::ApplyRequest a;
    a.freqHz = mhz * 1e6;
    a.profile = m_profile->currentData().toInt();
    a.rf = m_rfEnable->isChecked();
    a.power = m_power->currentIndex();
    a.sendExt = m_sendExt->isChecked();
    a.agcMode = m_agc->currentData().toInt();
    a.attenCode = static_cast<int>(std::lround(m_attenDb->value() * 2));
    a.targetMv = m_target->value();
    a.bandMode = m_bandMode->currentData().toInt();
    return a;
}

void VideoReceiverTab::applyNow(double mhz)
{
    if (!m_open || m_busy) {
        return;
    }
    m_freq->setValue(mhz);
    m_applyResult->setText(QStringLiteral("…"));
    const video::ApplyRequest a = requestFromUi(mhz);
    QMetaObject::invokeMethod(m_session, [s = m_session, a] { s->apply(a); }, Qt::QueuedConnection);
}

void VideoReceiverTab::showOp(const QString& op, bool ok, const QString& text)
{
    QLabel* target = op == QLatin1String("apply") ? m_applyResult
                     : op == QLatin1String("cp") ? m_cpResult
                     : op == QLatin1String("rtc") ? m_rtcResult
                     : op == QLatin1String("regs") ? m_regsInfo
                     : op == QLatin1String("osd") ? m_osdResult : nullptr;
    if (target != nullptr) {
        target->setText(text);
        ui::setTone(target, ok ? ui::Tone::Ok : ui::Tone::Error);
    }
}

void VideoReceiverTab::showStatus(const video::Status& s)
{
    const bool boot = s.fwMode == 2;
    m_state->setText(video::stateName(s.state));
    ui::setTone(m_state, s.state == 5 ? ui::Tone::Error : s.state == 6 ? ui::Tone::Warn
                         : s.state == 4 ? ui::Tone::Ok : ui::Tone::Neutral);
    m_error->setText(s.lastError ? QStringLiteral("LAST_ERROR: ")
                                       + (boot ? video::fwErrorName(s.lastError) : video::errorName(s.lastError))
                                 : QString());
    QStringList flags;
    if (boot) {
        flags << QStringLiteral("ЗАГРУЗЧИК · ") + video::fwStateName(s.fwupState);
    } else {
        flags << (s.flags & video::FlagPllLock ? QStringLiteral("PLL захвачен") : QStringLiteral("PLL не захвачен"))
              << (s.flags & video::FlagVideoLoss ? QStringLiteral("нет видео") : QStringLiteral("видео есть"))
              << (s.flags & video::FlagOsdReady ? QStringLiteral("OSD готов") : QStringLiteral("OSD не готов"));
        if (s.flags & video::FlagWatchdogSafe) {
            flags << QStringLiteral("SAFE по сторожу");
        }
    }
    m_flags->setText(flags.join(QStringLiteral(" · ")));
    const auto mhz = [](quint64 hz) {
        return hz ? QStringLiteral("%1 МГц").arg(static_cast<double>(hz) / 1e6, 0, 'f', hz % 1000 ? 6 : 3) : QStringLiteral("—");
    };
    QStringList v(15, QStringLiteral("—"));
    v[0] = boot ? QStringLiteral("загрузчик") : QStringLiteral("приложение");
    v[1] = s.fwText() + QStringLiteral(" / ") + s.blText();
    v[11] = s.bootText();
    v[12] = QStringLiteral("%1 с").arg(s.uptimeMs / 1000.0, 0, 'f', 1);
    v[13] = QStringLiteral("%1 / %2").arg(s.acceptedSeq).arg(s.appliedSeq);
    v[14] = QString::number(s.epoch);
    if (s.hasExt) {
        static const QStringList pwr{QStringLiteral("−4"), QStringLiteral("−1"), QStringLiteral("+2"), QStringLiteral("+5")};
        v[2] = (s.profile == 2 ? QStringLiteral("LO ") : QString()) + mhz(s.freqHz);
        v[3] = mhz(s.calcHz);
        v[4] = (s.band ? QStringLiteral("HFCN-5500 (5,5–6,0 ГГц)") : QStringLiteral("HFCN-4600 (4,9–5,5 ГГц)"))
               + (s.bandMode > 0 ? QStringLiteral(" · вручную") : s.bandMode == 0 ? QStringLiteral(" · авто") : QString());
        // Нижняя настройка: в профиле 1 задаётся частота сигнала, LO = F − 480 МГц.
        const double lo = s.calcHz ? (s.profile == 2 ? static_cast<double>(s.calcHz)
                                                     : static_cast<double>(s.calcHz) - video::kIfHz) : 0.0;
        v[5] = s.rfEnable ? (lo > 0 ? QStringLiteral("%1 МГц, ").arg(lo / 1e6, 0, 'f', 3) : QString())
                                + pwr.value(s.power, QStringLiteral("?")) + QStringLiteral(" дБм")
                          : QStringLiteral("выкл");
        v[6] = QStringLiteral("%1 мВ").arg(s.rssiMv);
        v[7] = QStringLiteral("%1 дБ").arg(s.attenCode / 2.0, 0, 'f', 1);
        v[8] = s.agcMode ? QStringLiteral("ручной") : QStringLiteral("авто %1 мВ").arg(s.targetMv);
        v[9] = QStringLiteral("0x%1").arg(s.osdStat, 2, 16, QLatin1Char('0'))
               + (s.osdStat & 4 ? QStringLiteral(" LOS") : s.osdStat & 1 ? QStringLiteral(" PAL")
                  : s.osdStat & 2 ? QStringLiteral(" NTSC") : QString());
        if (s.rtcPaths < 0) {
            v[10] = QStringLiteral("нет в ПО");
        } else {
            v[10] = QStringLiteral("%1 / %2 · 0x%3")
                        .arg(s.rtcPaths & 1 ? QStringLiteral("вкл") : QStringLiteral("выкл"),
                             s.rtcPaths & 2 ? QStringLiteral("вкл") : QStringLiteral("выкл"),
                             QString::number(s.rtcPd, 16).toUpper().rightJustified(5, QLatin1Char('0')));
            if (s.hasRtcRead) {
                if (s.rtcRead == s.rtcPd) {
                    v[10] += QStringLiteral(" ✓");
                } else if (s.rtcRead == 0) {
                    v[10] += QStringLiteral(" · RTC6715 не отвечает");
                } else if (s.rtcRead != 0xFFFFFFFFu) {
                    v[10] += QStringLiteral(" · прочитано 0x") + QString::number(s.rtcRead, 16).toUpper();
                }
            }
        }
        m_chart->add(s.rssiMv, s.attenCode / 2.0);
    }
    for (int i = 0; i < m_values.size(); ++i) {
        m_values.at(i)->setText(v.at(i));
    }
}

void VideoReceiverTab::chooseFirmware()
{
    QSettings s;
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Прошивка видеоприёмника"),
                                                      s.value(settingKey("fwDir")).toString(),
                                                      QStringLiteral("Образ приложения (*.bin)"));
    if (path.isEmpty()) {
        return;
    }
    s.setValue(settingKey("fwDir"), QFileInfo(path).absolutePath());
    QFile file(path);
    // Образ приложения не больше 54 КБ: читаем с запасом и не тянем в память чужой большой файл.
    if (!file.open(QIODevice::ReadOnly) || file.size() > 4 * video::kMaxImageBytes) {
        m_image.clear();
        m_fwInfo->setText(QStringLiteral("файл не читается или слишком велик"));
        ui::setTone(m_fwInfo, ui::Tone::Error);
        updateControls();
        return;
    }
    const video::FirmwareImage info = video::inspectFirmware(file.readAll());
    m_image = info.image;
    m_fwPath->setText(path);
    m_fwInfo->setText(info.error.isEmpty() ? info.summary : info.summary + QStringLiteral(" — ") + info.error);
    ui::setTone(m_fwInfo, info.error.isEmpty() ? ui::Tone::Info : ui::Tone::Error);
    updateControls();
}

QString VideoReceiverTab::reportText(const TestReport& r) const
{
    QStringList lines{
        QStringLiteral("ПРОТОКОЛ ПРОВЕРКИ — модуль видеоприёма Э.468116.00133"),
        QStringLiteral("Дата/время:   ") + r.time,
        QStringLiteral("Зав. номер:   ") + (r.serial.isEmpty() ? QStringLiteral("—") : r.serial),
        QStringLiteral("Проверяющий:  ") + (r.operatorName.isEmpty() ? QStringLiteral("—") : r.operatorName),
        QStringLiteral("Версия ПО:    %1, загрузчик %2").arg(r.identity.fwText(), r.identity.blText()),
        QStringLiteral("BOOT_ID:      ") + r.identity.bootText(),
        QStringLiteral("Тестовый сигнал: ") + r.signal,
        QString(),
    };
    for (const TestRow& row : r.rows) {
        lines << row.result.leftJustified(5) + QLatin1Char(' ') + row.name
                     + (row.text.isEmpty() ? QString() : QStringLiteral(" — ") + row.text);
    }
    lines << QString() << QStringLiteral("ИТОГ: ") + r.verdict;
    return lines.join(QStringLiteral("\r\n"));
}

void VideoReceiverTab::saveReport()
{
    if (m_history.isEmpty()) {
        return;
    }
    const TestReport& r = m_history.first();
    const QString name = QStringLiteral("%1/protokol_%2_%3.txt")
                             .arg(documentsDir(), r.serial.isEmpty() ? QStringLiteral("bez_nomera") : r.serial, stamp());
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Протокол проверки"), name,
                                                      QStringLiteral("Текст (*.txt)"));
    if (path.isEmpty()) {
        return;
    }
    QSettings().setValue(QStringLiteral("video/report_path"), path);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)
        || file.write("\xEF\xBB\xBF" + reportText(r).toUtf8()) < 0) {
        m_verdict->setText(QStringLiteral("протокол не записан: ") + file.errorString());
        ui::setTone(m_verdict, ui::Tone::Error);
    }
}

void VideoReceiverTab::saveHistory()
{
    if (m_history.isEmpty()) {
        return;
    }
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Проверенные платы"),
                                                      documentsDir() + QStringLiteral("/provereno.csv"),
                                                      QStringLiteral("CSV (*.csv)"));
    if (path.isEmpty()) {
        return;
    }
    QSettings().setValue(QStringLiteral("video/history_path"), path);
    const auto q = [](QString v) { return QLatin1Char('"') + v.replace(QLatin1Char('"'), QStringLiteral("\"\"")) + QLatin1Char('"'); };
    QStringList lines{QStringLiteral("Время;Зав. номер;Проверяющий;BOOT_ID;ПО;Итог;Ошибки")};
    for (const TestReport& r : m_history) {
        QStringList failed;
        for (const TestRow& row : r.rows) {
            if (row.result == QLatin1String("FAIL")) {
                failed << row.name;
            }
        }
        lines << QStringList{q(r.time), q(r.serial), q(r.operatorName), q(r.identity.bootText()), q(r.identity.fwText()),
                             q(r.verdict), q(failed.join(QStringLiteral(", ")))}.join(QLatin1Char(';'));
    }
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)
        || file.write("\xEF\xBB\xBF" + lines.join(QStringLiteral("\r\n")).toUtf8()) < 0) {
        m_verdict->setText(QStringLiteral("CSV не записан: ") + file.errorString());
        ui::setTone(m_verdict, ui::Tone::Error);
    }
}
