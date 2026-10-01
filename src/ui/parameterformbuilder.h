#pragma once

#include "document.h"

#include <QObject>
#include <QVector3D>

#include <functional>
#include <vector>

class QFormLayout;
class QLabel;
class QToolButton;
class QWidget;

// Turns a list of MeshFilterParameterDescriptor into editor rows in a QFormLayout,
// and reads the edited values back out.
//
// Shared by the filter panel and the preferences dialog: both describe their inputs
// with the same descriptors, so both get the same editors, grouping, help and
// advanced-section handling without duplicating any of it. See
// docs/design/adding_a_filter.md for the descriptor schema.
class ParameterFormBuilder : public QObject
{
    Q_OBJECT
public:
    struct ViewContext {
        QVector3D trackballCenter;
        QVector3D eyePosition;
        QVector3D viewDirection;
    };

    // Everything the document-coupled editors need. A caller with no document — the
    // preferences dialog — leaves this default-constructed, and the parameter types
    // that require one are skipped rather than built half-working.
    struct Context {
        Document *doc = nullptr;
        std::function<ViewContext()> viewContextProvider;
        std::function<QString()> cameraStateProvider;
        std::function<QString()> renderStateProvider;
    };

    // A group heading and whether it belongs to the advanced set, so that hiding the
    // advanced parameters hides their heading too.
    struct GroupHeading {
        QWidget *label = nullptr;
        bool advanced = false;
    };

    struct Binding {
        MeshFilterParameterDescriptor descriptor;
        QWidget *editor = nullptr;
        QWidget *formLabel = nullptr;
        // What the row holds: the editor alone, or the editor with its reset button.
        QWidget *field = nullptr;
        QToolButton *resetButton = nullptr;
        QLabel *helpLabel = nullptr;
        bool advanced = false;
    };

    // `layout` receives the generated rows; `parentWidget` parents the editors.
    ParameterFormBuilder(QFormLayout *layout, QWidget *parentWidget, QObject *parent = nullptr);

    void setContext(Context context);

    // Presentation options for build(), both off by default, as the filter panel wants
    // them. Inline help puts each parameter's help under its row as text, in place of the
    // tooltip it otherwise gets; reset buttons give each row a button, shown once its
    // value differs from the default, that puts that one value back.
    void setShowsInlineHelp(bool shows) { m_showsInlineHelp = shows; }
    void setShowsResetButtons(bool shows) { m_showsResetButtons = shows; }
    // Hides or shows that inline help, before or after build(), without a rebuild.
    void setInlineHelpVisible(bool visible);
    // Group headings are bold at the rows' size, so in the filter panel they rank below the
    // filter's bold title. A form whose groups are its top level, as in the preferences
    // dialog, sets them larger.
    void setGroupHeadingScale(double scale) { m_groupHeadingScale = scale; }

    // Removes every row from the layout and drops all bindings.
    void clear();

    // Builds one row per descriptor, in order, with a bold header whenever the
    // group changes (only when there is more than one group). Values present in
    // `initialValues` override the descriptor defaults, so a caller holding stored
    // settings can seed the form; anything absent falls back to the default.
    void build(
        const std::vector<MeshFilterParameterDescriptor> &parameters,
        const MeshFilterParameterValues &initialValues = {});

    MeshFilterParameterValues values() const;
    QVariant value(const QString &parameterId) const;
    void setValues(const MeshFilterParameterValues &values);
    void resetToDefaults();
    // Whether the editor shows the descriptor default, with the same tolerance the
    // preferences store uses (sameParameterValue).
    bool isDefault(const QString &parameterId) const;

    bool hasAdvanced() const { return m_hasAdvanced; }
    void setAdvancedVisible(bool visible);

    // Re-point every texture editor at the mesh named by its
    // `textureSourceMeshParameter` (or the current mesh when it names none).
    // Called automatically after build() and whenever a Mesh parameter changes.
    void refreshDependentEditors();

    // Re-evaluate every descriptor's `enabledWhen` gate. Called automatically after
    // build() and after any value changes.
    void refreshEnabledState();

    const std::vector<Binding> &bindings() const { return m_bindings; }
    const Binding *bindingById(const QString &parameterId) const;

signals:
    // Emitted whenever the user edits an editor; `parameterId` names the one edited.
    // Callers that need to react per keystroke (live-apply preferences) and callers
    // that only re-validate (the filter panel) both hang off this.
    void valueChanged(const QString &parameterId);

private:
    QWidget *createEditor(const MeshFilterParameterDescriptor &param);
    bool evaluateEnabledWhen(const QString &expression) const;
    void connectEditorSignals(const Binding &binding);
    void applyValue(const Binding &binding, const QVariant &value);
    QVariant readValue(const Binding &binding) const;
    void refreshResetButtons();

    QFormLayout *m_layout = nullptr;
    QWidget *m_parentWidget = nullptr;
    Context m_context;
    std::vector<Binding> m_bindings;
    std::vector<GroupHeading> m_groupHeadings;
    bool m_hasAdvanced = false;
    bool m_advancedVisible = false;
    bool m_showsInlineHelp = false;
    bool m_inlineHelpVisible = true;
    bool m_showsResetButtons = false;
    double m_groupHeadingScale = 1.0;
};
