#pragma once

#include "PsProtocol.h"

#include <QWidget>

#include <map>
#include <string>
#include <vector>

class PsSession;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;

/// Вкладка «OSD» в «Модулях»: наложение служебной информации видеоприёмника (v8 п. 3.3).
/// set_osd / get_osd принимаются в любом режиме; отладочный текст set_osd_text /
/// clear_osd_text — только в SERVICE (п. 3.6.1.11–3.6.1.12).
class PsOsdPage final : public QWidget {
    Q_OBJECT

public:
    explicit PsOsdPage(PsSession* session, QWidget* parent = nullptr);

    /// connected — есть связь с PS (set_osd/get_osd), service — режим SERVICE (текст).
    void setAllowed(bool connected, bool service);

    /// Параметры с формы для set_osd: все поля таблицы 13.
    [[nodiscard]] afar::ps::OsdParams formParams() const;
    bool writeOsd();
    bool readOsd();
    bool writeText();
    bool clearText();

private:
    void onResponse(const afar::ps::Envelope& r, afar::ps::Status status);
    void showParams(const afar::ps::OsdParams& p);
    void updateRowLimit();
    bool sendTracked(std::string_view type, const nlohmann::json& payload);

    PsSession* m_session;
    QCheckBox* m_en = nullptr;
    QComboBox* m_std = nullptr;
    QComboBox* m_src = nullptr;
    std::vector<QCheckBox*> m_fields;  ///< по osdFieldIds()
    QSpinBox* m_hos = nullptr;
    QSpinBox* m_vos = nullptr;
    QLabel* m_los = nullptr;
    QPushButton* m_write = nullptr;
    QPushButton* m_read = nullptr;

    QSpinBox* m_row = nullptr;
    QSpinBox* m_col = nullptr;
    QLineEdit* m_text = nullptr;
    QPushButton* m_writeText = nullptr;
    QPushButton* m_clearText = nullptr;

    QLabel* m_result = nullptr;
    std::map<int, std::string> m_pending;  ///< msg_count → msg_type
};
