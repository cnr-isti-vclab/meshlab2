// ParameterFormBuilder is the descriptor->editor layer shared by the filter panel
// and the preferences dialog. It used to be a switch buried inside MeshFilterPanel,
// which linked against the whole app and so was never covered; extracted, it needs
// only Document + Qt Widgets and can be exercised directly.
#include "parameterformbuilder.h"

#include "document.h"
#include "vcgmesh.h"

#include <QFormLayout>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QLineEdit>
#include <QSpinBox>
#include <QSignalSpy>
#include <QTest>
#include <QWidget>

namespace {

MeshFilterParameterDescriptor makeParam(
    const QString &id,
    MeshFilterParameterType type,
    const QVariant &defaultValue,
    const QString &group = QStringLiteral("main"))
{
    MeshFilterParameterDescriptor p;
    p.id = id;
    p.label = id;
    p.type = type;
    p.defaultValue = defaultValue;
    p.group = group;
    return p;
}

std::vector<MeshFilterParameterDescriptor> sampleParameters()
{
    auto intParam = makeParam(QStringLiteral("count"), MeshFilterParameterType::Int, 7);
    intParam.minValue = 0;
    intParam.maxValue = 100;

    auto doubleParam =
        makeParam(QStringLiteral("ratio"), MeshFilterParameterType::Double, 0.25, QStringLiteral("advanced"));
    doubleParam.minValue = 0.0;
    doubleParam.maxValue = 1.0;

    auto enumParam =
        makeParam(QStringLiteral("mode"), MeshFilterParameterType::Enum, QStringLiteral("b"));
    enumParam.enumOptions = {
        { QStringLiteral("a"), QStringLiteral("Alpha") },
        { QStringLiteral("b"), QStringLiteral("Beta") },
    };

    return {
        makeParam(QStringLiteral("enabled"), MeshFilterParameterType::Bool, true),
        intParam,
        doubleParam,
        makeParam(QStringLiteral("name"), MeshFilterParameterType::String, QStringLiteral("hello")),
        enumParam,
    };
}

} // namespace

class ParameterFormTests : public QObject
{
    Q_OBJECT
private slots:
    void buildsEditorsFromDefaults();
    void initialValuesOverrideDefaults();
    void reportsEditedValues();
    void emitsValueChangedOnEdit();
    void advancedVisibilityFollowsGroup();
    void groupHeadingsAppearOncePerGroup();
    void resetRestoresDefaults();
    void skipsDocumentTypesWithoutADocument();
    void enabledWhenGatesOnABool();
    void enabledWhenSupportsNegation();
    void enabledWhenIgnoresBadReferences();
    void point3fRoleChoosesThePresetList();
    void optionalMeshReferenceOffersNone();
};

void ParameterFormTests::buildsEditorsFromDefaults()
{
    QWidget host;
    auto *layout = new QFormLayout(&host);
    ParameterFormBuilder builder(layout, &host);
    builder.build(sampleParameters());

    const MeshFilterParameterValues values = builder.values();
    QCOMPARE(values.value(QStringLiteral("enabled")).toBool(), true);
    QCOMPARE(values.value(QStringLiteral("count")).toInt(), 7);
    QCOMPARE(values.value(QStringLiteral("ratio")).toDouble(), 0.25);
    QCOMPARE(values.value(QStringLiteral("name")).toString(), QStringLiteral("hello"));
    QCOMPARE(values.value(QStringLiteral("mode")).toString(), QStringLiteral("b"));
}

void ParameterFormTests::initialValuesOverrideDefaults()
{
    QWidget host;
    auto *layout = new QFormLayout(&host);
    ParameterFormBuilder builder(layout, &host);

    MeshFilterParameterValues stored;
    stored.insert(QStringLiteral("count"), 42);
    stored.insert(QStringLiteral("mode"), QStringLiteral("a"));
    builder.build(sampleParameters(), stored);

    // Seeded ids take the stored value, the rest keep their descriptor default.
    QCOMPARE(builder.value(QStringLiteral("count")).toInt(), 42);
    QCOMPARE(builder.value(QStringLiteral("mode")).toString(), QStringLiteral("a"));
    QCOMPARE(builder.value(QStringLiteral("ratio")).toDouble(), 0.25);
}

void ParameterFormTests::reportsEditedValues()
{
    QWidget host;
    auto *layout = new QFormLayout(&host);
    ParameterFormBuilder builder(layout, &host);
    builder.build(sampleParameters());

    auto *spin = qobject_cast<QSpinBox *>(builder.bindingById(QStringLiteral("count"))->editor);
    QVERIFY(spin);
    spin->setValue(13);

    auto *line = qobject_cast<QLineEdit *>(builder.bindingById(QStringLiteral("name"))->editor);
    QVERIFY(line);
    line->setText(QStringLiteral("edited"));

    QCOMPARE(builder.values().value(QStringLiteral("count")).toInt(), 13);
    QCOMPARE(builder.values().value(QStringLiteral("name")).toString(), QStringLiteral("edited"));
}

void ParameterFormTests::emitsValueChangedOnEdit()
{
    QWidget host;
    auto *layout = new QFormLayout(&host);
    ParameterFormBuilder builder(layout, &host);
    builder.build(sampleParameters());

    QSignalSpy spy(&builder, &ParameterFormBuilder::valueChanged);

    qobject_cast<QCheckBox *>(builder.bindingById(QStringLiteral("enabled"))->editor)->setChecked(false);
    qobject_cast<QDoubleSpinBox *>(builder.bindingById(QStringLiteral("ratio"))->editor)->setValue(0.5);
    qobject_cast<QComboBox *>(builder.bindingById(QStringLiteral("mode"))->editor)->setCurrentIndex(0);

    QCOMPARE(spy.count(), 3);
    QCOMPARE(spy.at(0).at(0).toString(), QStringLiteral("enabled"));
    QCOMPARE(spy.at(1).at(0).toString(), QStringLiteral("ratio"));
    QCOMPARE(spy.at(2).at(0).toString(), QStringLiteral("mode"));
}

void ParameterFormTests::advancedVisibilityFollowsGroup()
{
    QWidget host;
    auto *layout = new QFormLayout(&host);
    ParameterFormBuilder builder(layout, &host);
    builder.build(sampleParameters());
    host.show();

    QVERIFY(builder.hasAdvanced());
    const auto *advanced = builder.bindingById(QStringLiteral("ratio"));
    const auto *plain = builder.bindingById(QStringLiteral("count"));
    QVERIFY(advanced->advanced);
    QVERIFY(!plain->advanced);

    // Advanced rows start hidden; the plain ones are never touched by the toggle.
    QVERIFY(advanced->editor->isHidden());
    builder.setAdvancedVisible(true);
    QVERIFY(!advanced->editor->isHidden());
    QVERIFY(!plain->editor->isHidden());
    builder.setAdvancedVisible(false);
    QVERIFY(advanced->editor->isHidden());
    QVERIFY(!plain->editor->isHidden());
}

// Headings used to be emitted whenever the group changed while walking the descriptor
// order, so a filter declaring main/advanced/main/advanced showed four headings for two
// groups -- and the "Advanced" ones stayed on screen above nothing while the advanced
// parameters were hidden.
void ParameterFormTests::groupHeadingsAppearOncePerGroup()
{
    auto param = [](const QString &id, const QString &group) {
        MeshFilterParameterDescriptor p;
        p.id = id;
        p.label = id;
        p.group = group;
        p.type = MeshFilterParameterType::Bool;
        p.defaultValue = false;
        return p;
    };
    // Interleaved on purpose: this is the ordering that produced the duplicates.
    const std::vector<MeshFilterParameterDescriptor> params{
        param(QStringLiteral("a"), QStringLiteral("main")),
        param(QStringLiteral("b"), QStringLiteral("advanced")),
        param(QStringLiteral("c"), QStringLiteral("main")),
        param(QStringLiteral("d"), QStringLiteral("advanced")),
    };

    QWidget host;
    auto *layout = new QFormLayout(&host);
    ParameterFormBuilder builder(layout, &host);
    builder.build(params);
    host.show();

    const auto headings = [&]() {
        QStringList out;
        for (int row = 0; row < layout->rowCount(); ++row) {
            QLayoutItem *spanning = layout->itemAt(row, QFormLayout::SpanningRole);
            if (!spanning || !spanning->widget())
                continue;
            auto *label = qobject_cast<QLabel *>(spanning->widget());
            if (label && !label->isHidden())
                out << label->text();
        }
        return out;
    };

    builder.setAdvancedVisible(true);
    QCOMPARE(headings(), (QStringList{ QStringLiteral("Main"), QStringLiteral("Advanced") }));

    // With the advanced parameters hidden only one group is left, and a lone heading
    // says nothing the form does not already say.
    builder.setAdvancedVisible(false);
    QCOMPARE(headings(), QStringList{});

    // The parameters themselves keep their declared order inside each group.
    QVERIFY(builder.bindingById(QStringLiteral("a")) != nullptr);
    QVERIFY(builder.bindingById(QStringLiteral("c")) != nullptr);
    QVERIFY(builder.bindingById(QStringLiteral("b"))->advanced);
}

void ParameterFormTests::resetRestoresDefaults()
{
    QWidget host;
    auto *layout = new QFormLayout(&host);
    ParameterFormBuilder builder(layout, &host);
    builder.build(sampleParameters());

    qobject_cast<QSpinBox *>(builder.bindingById(QStringLiteral("count"))->editor)->setValue(99);
    qobject_cast<QComboBox *>(builder.bindingById(QStringLiteral("mode"))->editor)->setCurrentIndex(0);
    QCOMPARE(builder.value(QStringLiteral("count")).toInt(), 99);

    builder.resetToDefaults();
    QCOMPARE(builder.value(QStringLiteral("count")).toInt(), 7);
    QCOMPARE(builder.value(QStringLiteral("mode")).toString(), QStringLiteral("b"));
}

// A caller with no Document (the preferences dialog) must still get a usable form:
// the mesh/texture/state types are skipped rather than built against a null document.
void ParameterFormTests::skipsDocumentTypesWithoutADocument()
{
    QWidget host;
    auto *layout = new QFormLayout(&host);
    ParameterFormBuilder builder(layout, &host);

    std::vector<MeshFilterParameterDescriptor> params = sampleParameters();
    params.push_back(makeParam(QStringLiteral("srcMesh"), MeshFilterParameterType::Mesh, 0));
    params.push_back(makeParam(QStringLiteral("tex"), MeshFilterParameterType::TextureRef, 0));
    params.push_back(
        makeParam(QStringLiteral("cam"), MeshFilterParameterType::CameraState, QString()));
    builder.build(params);

    QCOMPARE(builder.bindings().size(), sampleParameters().size());
    QVERIFY(builder.bindingById(QStringLiteral("srcMesh")) == nullptr);
    QVERIFY(builder.bindingById(QStringLiteral("tex")) == nullptr);
    QVERIFY(builder.bindingById(QStringLiteral("cam")) == nullptr);
    QVERIFY(builder.bindingById(QStringLiteral("count")) != nullptr);
}

void ParameterFormTests::enabledWhenGatesOnABool()
{
    QWidget host;
    auto *layout = new QFormLayout(&host);
    ParameterFormBuilder builder(layout, &host);

    auto gate = makeParam(QStringLiteral("useRange"), MeshFilterParameterType::Bool, false);
    auto gated = makeParam(QStringLiteral("rangeMax"), MeshFilterParameterType::Double, 1.0);
    gated.enabledWhen = QStringLiteral("useRange");
    builder.build({ gate, gated });

    // Gate starts false, so the dependent row starts disabled...
    QVERIFY(!builder.bindingById(QStringLiteral("rangeMax"))->editor->isEnabled());
    QVERIFY(!builder.bindingById(QStringLiteral("rangeMax"))->formLabel->isEnabled());

    // ...and follows the gate without the caller doing anything.
    qobject_cast<QCheckBox *>(builder.bindingById(QStringLiteral("useRange"))->editor)
        ->setChecked(true);
    QVERIFY(builder.bindingById(QStringLiteral("rangeMax"))->editor->isEnabled());
    QVERIFY(builder.bindingById(QStringLiteral("rangeMax"))->formLabel->isEnabled());
}

void ParameterFormTests::enabledWhenSupportsNegation()
{
    QWidget host;
    auto *layout = new QFormLayout(&host);
    ParameterFormBuilder builder(layout, &host);

    auto gate = makeParam(QStringLiteral("automatic"), MeshFilterParameterType::Bool, true);
    auto gated = makeParam(QStringLiteral("manualValue"), MeshFilterParameterType::Int, 3);
    gated.enabledWhen = QStringLiteral("!automatic");
    builder.build({ gate, gated });

    QVERIFY(!builder.bindingById(QStringLiteral("manualValue"))->editor->isEnabled());
    qobject_cast<QCheckBox *>(builder.bindingById(QStringLiteral("automatic"))->editor)
        ->setChecked(false);
    QVERIFY(builder.bindingById(QStringLiteral("manualValue"))->editor->isEnabled());
}

// A typo must not leave a control greyed out with no way to re-enable it.
void ParameterFormTests::enabledWhenIgnoresBadReferences()
{
    QWidget host;
    auto *layout = new QFormLayout(&host);
    ParameterFormBuilder builder(layout, &host);

    auto missingGate = makeParam(QStringLiteral("a"), MeshFilterParameterType::Int, 1);
    missingGate.enabledWhen = QStringLiteral("noSuchParameter");
    auto nonBoolGate = makeParam(QStringLiteral("b"), MeshFilterParameterType::Int, 1);
    nonBoolGate.enabledWhen = QStringLiteral("a");
    builder.build({ missingGate, nonBoolGate });

    QVERIFY(builder.bindingById(QStringLiteral("a"))->editor->isEnabled());
    QVERIFY(builder.bindingById(QStringLiteral("b"))->editor->isEnabled());
}

// The three roles offer different presets, and an axis deliberately drops the sign:
// a cylinder along +X is the same cylinder as one along -X, so listing both is noise.
// Roles that do care about the sign (camera directions) keep the full list.
void ParameterFormTests::point3fRoleChoosesThePresetList()
{
    const auto presetsFor = [](const QString &role) {
        QWidget host;
        auto *layout = new QFormLayout(&host);
        ParameterFormBuilder builder(layout, &host);
        auto p = makeParam(QStringLiteral("dir"), MeshFilterParameterType::Point3f,
                           QVector3D(0, 1, 0));
        p.point3fRole = role;
        builder.build({ p });
        QStringList labels;
        auto *combo = builder.bindingById(QStringLiteral("dir"))
                          ->editor->findChild<QComboBox *>();
        for (int i = 0; combo && i < combo->count(); ++i)
            labels << combo->itemText(i);
        return labels;
    };

    QCOMPARE(presetsFor(QStringLiteral("axis")),
             QStringList({ QStringLiteral("Custom"), QStringLiteral("X Axis"),
                           QStringLiteral("Y Axis"), QStringLiteral("Z Axis"),
                           QStringLiteral("View Direction") }));
    QVERIFY(presetsFor(QStringLiteral("direction")).contains(QStringLiteral("-X Axis")));
    // Unspecified means "point", which offers places rather than orientations.
    QVERIFY(presetsFor(QString()).contains(QStringLiteral("Mesh BBox Center")));
}

// A mesh reference marked meshAllowsNone offers an explicit empty choice and can default
// to it. Without the flag a mesh parameter can never be left unset, which would stop a
// filter like Create Grid -- whose layer reference is only there to take a size from --
// running at all on a document with no layers to point at.
void ParameterFormTests::optionalMeshReferenceOffersNone()
{
    const auto comboFor = [](ParameterFormBuilder &b, const QString &id) {
        const ParameterFormBuilder::Binding *binding = b.bindingById(id);
        return binding ? qobject_cast<QComboBox *>(binding->editor) : nullptr;
    };

    Document empty;
    QWidget host;
    auto *layout = new QFormLayout(&host);
    ParameterFormBuilder builder(layout, &host);
    builder.setContext({ &empty, {}, {}, {} });

    auto optional = makeParam(QStringLiteral("fitLayer"), MeshFilterParameterType::Mesh, -1);
    optional.meshAllowsNone = true;
    builder.build({ optional,
                    makeParam(QStringLiteral("srcMesh"), MeshFilterParameterType::Mesh, -1) });

    QComboBox *optionalCombo = comboFor(builder, QStringLiteral("fitLayer"));
    QVERIFY(optionalCombo != nullptr);
    QCOMPARE(optionalCombo->count(), 1);
    QCOMPARE(optionalCombo->itemData(0).toInt(), -1);
    QCOMPARE(builder.value(QStringLiteral("fitLayer")).toInt(), -1);

    // The mandatory one gets no empty row, so an empty document leaves it with nothing to
    // offer -- which is exactly why the optional flag has to exist.
    QComboBox *mandatoryCombo = comboFor(builder, QStringLiteral("srcMesh"));
    QVERIFY(mandatoryCombo != nullptr);
    QCOMPARE(mandatoryCombo->count(), 0);

    // With layers present the empty row is still first, still the default, and the layers
    // follow it at their own indices.
    Document doc;
    doc.addMesh(VCGMesh(), QStringLiteral("first"));
    doc.addMesh(VCGMesh(), QStringLiteral("second"));
    doc.setCurrentMeshIndex(1);

    QWidget host2;
    auto *layout2 = new QFormLayout(&host2);
    ParameterFormBuilder builder2(layout2, &host2);
    builder2.setContext({ &doc, {}, {}, {} });

    auto pinned = makeParam(QStringLiteral("pinned"), MeshFilterParameterType::Mesh, 0);
    pinned.meshAllowsNone = true;
    builder2.build({ optional, pinned,
                     makeParam(QStringLiteral("srcMesh"), MeshFilterParameterType::Mesh, -1) });

    QComboBox *withLayers = comboFor(builder2, QStringLiteral("fitLayer"));
    QVERIFY(withLayers != nullptr);
    QCOMPARE(withLayers->count(), 3);
    QCOMPARE(builder2.value(QStringLiteral("fitLayer")).toInt(), -1);
    QCOMPARE(builder2.value(QStringLiteral("pinned")).toInt(), 0);
    // A mandatory reference with a nonsense default still falls back to the current layer.
    QCOMPARE(builder2.value(QStringLiteral("srcMesh")).toInt(), 1);
}

QTEST_MAIN(ParameterFormTests)
#include "test_parameterform.moc"
