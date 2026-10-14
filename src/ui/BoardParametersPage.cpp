#include "BoardParametersPage.h"

#include "UiStyle.h"
#include "spectrum_engine.hpp"

#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <mutex>
#include <string>

BoardParametersPage::BoardParametersPage(SpectrumEngine& engine, MacProvider macProvider,
                                         IpProvider ipProvider, QWidget* parent)
    : QWidget(parent)
    , m_engine(engine)
    , m_macProvider(std::move(macProvider))
    , m_ipProvider(std::move(ipProvider))
{
    setObjectName(QStringLiteral("parametersWorkspace"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(ui::kInnerGap);

    auto* row = new QHBoxLayout;
    row->setSpacing(ui::kInnerGap);
    m_read = new QPushButton(QStringLiteral("Получить параметры"), this);
    m_read->setObjectName(QStringLiteral("readBoardParameters"));
    ui::setButtonRole(m_read, ui::ButtonRole::Primary);
    m_status = new QLabel(QStringLiteral("Готово к чтению"), this);
    m_status->setObjectName(QStringLiteral("boardParametersStatus"));
    m_status->setWordWrap(true);
    row->addWidget(m_status, 1);
    row->addWidget(m_read);

    m_results = new QTabWidget(this);
    m_results->setObjectName(QStringLiteral("boardParametersTabs"));
    const QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    m_main = new QPlainTextEdit(m_results);
    m_main->setObjectName(QStringLiteral("boardParametersOutput"));
    m_details = new QPlainTextEdit(m_results);
    m_details->setObjectName(QStringLiteral("boardParametersDetails"));
    for (QPlainTextEdit* edit : {m_main, m_details}) {
        edit->setReadOnly(true);
        edit->setLineWrapMode(QPlainTextEdit::NoWrap);
        edit->setFont(mono);
        edit->setMinimumHeight(120);
    }
    m_main->setPlaceholderText(QStringLiteral("После нажатия здесь появятся основные параметры платы."));
    m_details->setPlaceholderText(QStringLiteral("Здесь появится полный перечень технических IIO-атрибутов."));
    m_results->addTab(m_main, QStringLiteral("Основное"));
    m_results->addTab(m_details, QStringLiteral("Подробно"));

    layout->addLayout(row);
    layout->addWidget(m_results, 1);

    connect(m_read, &QPushButton::clicked, this, &BoardParametersPage::requestRead);
    m_timer = new QTimer(this);
    connect(m_timer, &QTimer::timeout, this, &BoardParametersPage::poll);
    m_timer->start(100);
}

void BoardParametersPage::requestRead()
{
    const QString mac = m_macProvider ? m_macProvider() : QString();
    const QString knownIp = m_ipProvider ? m_ipProvider() : QString();
    auto& state = m_engine.state();
    {
        std::lock_guard<std::mutex> lock(state.board_mutex);
        state.board_mac = mac.toStdString();
        state.board_known_ip = knownIp.toStdString();
        state.board_found_ip.clear();
        state.board_parameters.clear();
        state.board_status = "ожидание запуска поиска...";
    }
    state.board_read_parameters_requested.store(true);
    state.board_find_requested.store(true);
    m_busy = true;
    emit operationProgress(knownIp.isEmpty() ? QStringLiteral("Поиск платы по MAC")
                                             : QStringLiteral("Чтение параметров платы"),
                           0, 2);
    m_read->setEnabled(false);
    m_status->setText(knownIp.isEmpty() ? QStringLiteral("Поиск платы по MAC %1…").arg(mac)
                                        : QStringLiteral("Читаю параметры платы %1…").arg(knownIp));
    m_main->clear();
    m_details->clear();
    m_results->setCurrentIndex(0);
}

void BoardParametersPage::poll()
{
    auto& state = m_engine.state();
    std::string ip;
    std::string status;
    std::string parameters;
    {
        std::lock_guard<std::mutex> lock(state.board_mutex);
        ip = state.board_found_ip;
        status = state.board_status;
        parameters = state.board_parameters;
    }
    const bool searching = state.board_find_requested.load() || state.board_is_finding.load();
    m_read->setEnabled(!searching);
    if (!status.empty()) {
        m_status->setText(QString::fromStdString(status));
    }
    const QString snapshot = QString::fromStdString(parameters);
    if (snapshot != m_lastParameters) {
        m_lastParameters = snapshot;
        const QString detailsMarker = QStringLiteral("\nВСЕ СТАНДАРТНЫЕ ДОСТУПНЫЕ IIO-АТРИБУТЫ\n");
        const qsizetype detailsAt = snapshot.indexOf(detailsMarker);
        m_main->setPlainText(detailsAt < 0 ? snapshot.trimmed() : snapshot.left(detailsAt).trimmed());
        m_details->setPlainText(detailsAt < 0 ? QString()
                                              : snapshot.mid(detailsAt + detailsMarker.size()).trimmed());
    }
    if (m_busy) {
        if (searching) {
            emit operationProgress(ip.empty() ? QStringLiteral("Поиск платы по MAC")
                                              : QStringLiteral("Чтение параметров платы"),
                                   ip.empty() ? 0 : 1, 2);
        } else if (!status.empty()) {
            emit operationProgress(parameters.empty() ? QStringLiteral("Параметры платы не получены")
                                                      : QStringLiteral("Параметры платы получены"),
                                   2, 2);
            m_busy = false;
        }
    }
}
