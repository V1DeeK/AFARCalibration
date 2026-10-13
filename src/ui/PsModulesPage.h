#pragma once

#include "PsProtocol.h"

#include <QWidget>

#include <array>
#include <map>
#include <vector>

class PsLink;
class QCheckBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QTabWidget;

/// Страница «Модули»: команды внутренних модулей v3 §2.5.2 (контракт §4.5) —
/// гетеродины, смеситель, видеоприёмник. Программа шлёт только JSON; кадры
/// RS-485 собирает PS. В set_* уходят только отмеченные «Передать» поля.
class PsModulesPage final : public QWidget {
    Q_OBJECT

public:
    enum Module { Lo1, Lo2, Video, Mixer, ModuleCount };

    explicit PsModulesPage(PsLink* link, QWidget* parent = nullptr);

    /// Команды разрешены (связь есть, режим SERVICE, серии нет).
    void setAllowed(bool allowed);

    /// set_* по отмеченным полям; false — ничего не отмечено или нет связи.
    bool write(Module m);
    /// get_*; false — нет связи.
    bool read(Module m);
    /// Вкладки модулей: сюда же «Установка параметров» кладёт коды решётки.
    [[nodiscard]] QTabWidget* tabs() const { return m_tabs; }

private:
    /// Одно поле модуля: «Передать», значение, прочитанное из PS.
    struct FieldRow {
        afar::ps::ModuleField field;
        QCheckBox* send = nullptr;
        QWidget* editor = nullptr;
        QLabel* actual = nullptr;
    };
    struct ModuleUi {
        std::vector<FieldRow> rows;
        QLabel* state = nullptr;  ///< статус ответа, temp, lock
        QPushButton* write = nullptr;
        QPushButton* read = nullptr;
    };
    struct MixerRow {
        QCheckBox* send = nullptr;
        std::array<QWidget*, 4> editors{};  ///< en, rp, if_att, att — как mixerChannelFields()
        QLabel* actual = nullptr;
    };

    QWidget* buildModule(Module m, const std::vector<afar::ps::ModuleField>& fields);
    QWidget* buildMixer();
    QWidget* makeEditor(const afar::ps::ModuleField& f, const QString& name, QCheckBox* send);
    double editorValue(const afar::ps::ModuleField& f, QWidget* editor) const;
    void setEditorValue(const afar::ps::ModuleField& f, QWidget* editor, double v);
    void onResponse(const afar::ps::Envelope& r, afar::ps::Status status);
    void showModule(Module m, afar::ps::Status status, const nlohmann::json& payload);
    void showMixer(afar::ps::Status status, const nlohmann::json& payload);
    void sendCommand(Module m, std::string_view type, const nlohmann::json& payload);

    PsLink* m_link;
    QTabWidget* m_tabs = nullptr;
    std::array<ModuleUi, ModuleCount> m_ui{};
    std::array<MixerRow, afar::ps::kMixerChannels> m_mixer{};
    QLabel* m_result = nullptr;
    std::map<int, Module> m_pending;  ///< msg_count → модуль, чей ответ ждём
    bool m_filling = false;  ///< правка из ответа PS не отмечает «Передать»
};
