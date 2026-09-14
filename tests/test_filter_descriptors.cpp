#include <QtTest/QtTest>

#include <QDirIterator>
#include <QFile>
#include <QRegularExpression>
#include <QSet>

#include "document.h"
#include "filtercategories.h"

#include <vcg/complex/algorithms/create/platonic.h>
#include <vcg/complex/algorithms/update/bounding.h>
#include <vcg/complex/algorithms/update/normal.h>

#include <wrap/io_trimesh/io_mask.h>

// Descriptor conformance: every invariant that can be decided from a filter's
// manifest entry alone, checked once per filter, with the filter id as the row
// tag so a failure names the offender instead of the sweep.
//
// Nothing here loads a mesh into a filter or runs one. What it does need is a
// document, because descriptors are resolved against one: a bound token such as
// "@bboxDiagTenth" or "@qualityFMax" is only a number once there is a mesh to
// measure. The fixture below therefore has a real bounding box and a quality
// range at both vertex and face level, so bound ranges resolve to something
// meaningful and a parameter whose default falls outside its own range shows up
// -- which is how sample_offset_surface_recursively's Offset was found to be
// unusable at its default.
//
// The complementary sweep -- can every filter actually run -- belongs in a
// separate suite; this one stays fast (milliseconds) and covers all of them.

namespace {

// Recognised two-letter codes for MeshFilterDescriptor::outputModifies.
const QSet<QString> &outputModifyCodes()
{
    static const QSet<QString> codes = {
        QStringLiteral("VG"), QStringLiteral("VN"), QStringLiteral("VC"),
        QStringLiteral("VQ"), QStringLiteral("VT"), QStringLiteral("VA"),
        QStringLiteral("VS"), QStringLiteral("FV"), QStringLiteral("FN"),
        QStringLiteral("FC"), QStringLiteral("FQ"), QStringLiteral("FA"),
        QStringLiteral("FS"), QStringLiteral("FP"), QStringLiteral("WT"),
        QStringLiteral("TX"), QStringLiteral("TM")
    };
    return codes;
}

// Recognised codes for MeshFilterDescriptor::inputPrepare.
const QSet<QString> &inputPrepareCodes()
{
    static const QSet<QString> codes = {
        QStringLiteral("FF"), QStringLiteral("VF"), QStringLiteral("BorderFF"),
        QStringLiteral("BorderVF"), QStringLiteral("FNorm"), QStringLiteral("VNorm"),
        QStringLiteral("BBox"), QStringLiteral("FMark"), QStringLiteral("VMark"),
        QStringLiteral("Mark"), QStringLiteral("VTex"), QStringLiteral("VT"),
        QStringLiteral("WTex"), QStringLiteral("WT"), QStringLiteral("CurvDir")
    };
    return codes;
}

bool isNumericParameter(MeshFilterParameterType type)
{
    return type == MeshFilterParameterType::Int
        || type == MeshFilterParameterType::Double
        || type == MeshFilterParameterType::AbsPerc;
}

// A number, once tokens have been resolved. An unset or symbolic value is "no bound".
bool asBound(const QVariant &v, double &out)
{
    if (!v.isValid() || v.isNull())
        return false;
    if (v.userType() == QMetaType::QString)
        return false;
    bool ok = false;
    const double d = v.toDouble(&ok);
    if (!ok || !std::isfinite(d))
        return false;
    out = d;
    return true;
}

// A sphere carrying every attribute a descriptor can bind a range to, with
// quality varying at both vertex and face level so @qualityVMin/@qualityFMax
// resolve to a genuine interval rather than a degenerate point.
void buildFixture(VCGMesh &mesh)
{
    mesh.Clear();
    vcg::tri::Sphere(mesh, 3);
    mesh.vert.EnableTexCoord();
    mesh.face.EnableWedgeTexCoord();
    vcg::tri::UpdateBounding<VCGMesh>::Box(mesh);
    vcg::tri::UpdateNormal<VCGMesh>::PerVertexNormalizedPerFaceNormalized(mesh);

    for (auto &v : mesh.vert) {
        v.C() = vcg::Color4b(200, 180, 160, 255);
        v.Q() = v.P().X();
        v.T().U() = 0.5f + 0.4f * v.P().X();
        v.T().V() = 0.5f + 0.4f * v.P().Y();
    }
    for (size_t i = 0; i < mesh.face.size(); ++i) {
        auto &f = mesh.face[i];
        f.C() = vcg::Color4b(160, 180, 200, 255);
        f.Q() = float(i) / float(std::max<size_t>(1, mesh.face.size() - 1));
        f.WT(0) = VCGFace::TexCoordType(0.0f, 0.0f);
        f.WT(1) = VCGFace::TexCoordType(1.0f, 0.0f);
        f.WT(2) = VCGFace::TexCoordType(0.0f, 1.0f);
        for (int corner = 0; corner < 3; ++corner)
            f.WT(corner).N() = 0;
    }
}

int fixtureIoMask()
{
    using vcg::tri::io::Mask;
    return Mask::IOM_VERTCOORD | Mask::IOM_VERTNORMAL | Mask::IOM_VERTCOLOR
         | Mask::IOM_VERTQUALITY | Mask::IOM_VERTTEXCOORD
         | Mask::IOM_FACEINDEX | Mask::IOM_FACENORMAL | Mask::IOM_FACECOLOR
         | Mask::IOM_FACEQUALITY | Mask::IOM_WEDGTEXCOORD;
}

} // namespace

class FilterDescriptorTests : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void registryIsPopulated();
    void filterIdsAreUnique();
    void pythonNamesAreUnique();
    void descriptorConforms_data();
    void descriptorConforms();

private:
    // Parsed once: the ratified verbs of docs/design/vocabulary.md section 3.
    void loadLexicon();
    // Parsed once: every plugin source, concatenated, for the dispatchability check.
    void loadPluginSources();

    void checkIdentity(const MeshFilterDescriptor &d, QStringList &problems) const;
    void checkClassification(const MeshFilterDescriptor &d, QStringList &problems) const;
    void checkParameters(const MeshFilterDescriptor &d, QStringList &problems) const;
    void checkCodes(const MeshFilterDescriptor &d, QStringList &problems) const;
    void checkReferences(const MeshFilterDescriptor &d, QStringList &problems) const;

    Document m_doc;
    std::vector<Document::FilterInfo> m_infos;
    QSet<QString> m_verbs;
    QSet<QString> m_appliedRoots;
    QStringList m_namedResults;
    QString m_pluginSources;
};

void FilterDescriptorTests::initTestCase()
{
    VCGMesh fixture;
    buildFixture(fixture);
    // Two layers, so descriptors binding @otherMeshIndex resolve to a real one.
    m_doc.addMesh(fixture, QStringLiteral("fixture"), fixtureIoMask());
    m_doc.addMesh(fixture, QStringLiteral("fixture other"), fixtureIoMask());
    m_doc.setCurrentMeshIndex(0);

    m_infos = m_doc.filterInfos();
    QVERIFY2(!m_infos.empty(), "filter registry is empty");

    loadLexicon();
    loadPluginSources();
}

// Section 3 of the vocabulary lists the ratified verbs in a table, one row per
// verb (a couple of rows carry several). Below the `Estimate` footnote come the
// commentary blocks, which quote *rejected* words in backticked tables -- the
// vcglib `Update*` mapping -- so parsing past it would admit exactly the words
// the lexicon exists to keep out.
void FilterDescriptorTests::loadLexicon()
{
    QFile doc(QStringLiteral(TEST_SOURCE_DIR "/docs/design/vocabulary.md"));
    QVERIFY2(doc.open(QIODevice::ReadOnly | QIODevice::Text),
             "cannot open docs/design/vocabulary.md");
    const QString text = QString::fromUtf8(doc.readAll());
    doc.close();

    const int start = text.indexOf(QStringLiteral("## 3. Verb lexicon"));
    QVERIFY2(start >= 0, "vocabulary.md has no '## 3. Verb lexicon' section");
    int end = text.indexOf(QStringLiteral("\n## "), start + 1);
    if (end < 0)
        end = text.size();
    QString section = text.mid(start, end - start);

    const int ratifiedEnd = section.indexOf(QStringLiteral("`Estimate` is permitted"));
    QVERIFY2(ratifiedEnd > 0, "section 3 no longer carries the `Estimate` footnote that "
                              "marks the end of the ratified lexicon");
    section.truncate(ratifiedEnd);

    static const QRegularExpression rowRe(
        QStringLiteral("^\\|\\s*((?:`[A-Za-z]+`(?:,\\s*)?)+)\\s*\\|"));
    static const QRegularExpression tickRe(QStringLiteral("`([A-Za-z]+)`"));
    const QStringList lines = section.split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        const QRegularExpressionMatch m = rowRe.match(line);
        if (!m.hasMatch())
            continue;
        auto it = tickRe.globalMatch(m.captured(1));
        while (it.hasNext())
            m_verbs.insert(it.next().captured(1));
    }
    // The closed group of attribute-editing verbs is admitted as prose, not as rows.
    const int groupAt = section.indexOf(QStringLiteral("**Attribute-editing verbs**"));
    if (groupAt >= 0) {
        const int groupEnd = section.indexOf(QStringLiteral("\n\n"), groupAt);
        auto it = tickRe.globalMatch(section.mid(groupAt, groupEnd - groupAt));
        while (it.hasNext())
            m_verbs.insert(it.next().captured(1));
    }
    QVERIFY2(m_verbs.size() > 25,
             qPrintable(QStringLiteral("parsed only %1 verbs from the lexicon; the table "
                                       "format probably changed").arg(m_verbs.size())));
    QVERIFY(m_verbs.contains(QStringLiteral("Compute")));
    QVERIFY2(!m_verbs.contains(QStringLiteral("Update")), "`Update` is documented as rejected");

    // Roots whose renaming round has been applied. Extend as each round lands; the
    // pending ones are known not to conform yet and would only add noise here.
    m_appliedRoots = {
        QStringLiteral("Meshing"), QStringLiteral("Attribute"),
        QStringLiteral("Creation"), QStringLiteral("Geometry"),
        QStringLiteral("Selection"), QStringLiteral("Repair"),
        QStringLiteral("Document"), QStringLiteral("Parametrization"),
        QStringLiteral("Measurement"), QStringLiteral("Transfer"),
        QStringLiteral("Texture")
    };
    // The named-result exception in vocabulary.md section 6: a filter whose output
    // *is* a conventionally named object may be a noun phrase.
    m_namedResults = {
        QStringLiteral("Mesh Union"), QStringLiteral("Mesh Intersection"),
        QStringLiteral("Mesh Difference"), QStringLiteral("Mesh Symmetric Difference"),
        QStringLiteral("Mesh CSG Expression")
    };
}

void FilterDescriptorTests::loadPluginSources()
{
    QDirIterator it(QStringLiteral(TEST_SOURCE_DIR "/plugins"),
                    {QStringLiteral("*.cpp"), QStringLiteral("*.h")},
                    QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        // Vendored upstream trees carry their own unrelated string literals.
        if (path.contains(QStringLiteral("/upstream/")))
            continue;
        QFile f(path);
        if (f.open(QIODevice::ReadOnly))
            m_pluginSources += QString::fromUtf8(f.readAll());
    }
    QVERIFY2(m_pluginSources.size() > 100000, "could not read the plugin sources");
}

void FilterDescriptorTests::registryIsPopulated()
{
    // A registry that silently shrinks -- a plugin dropped from the build, a manifest
    // that failed to parse -- would make every other row in this suite vacuous.
    QVERIFY2(m_infos.size() > 300,
             qPrintable(QStringLiteral("only %1 filters registered").arg(m_infos.size())));
}

void FilterDescriptorTests::filterIdsAreUnique()
{
    QSet<QString> seen;
    QStringList duplicates;
    for (const auto &info : m_infos) {
        if (seen.contains(info.descriptor.id))
            duplicates << info.descriptor.id;
        seen.insert(info.descriptor.id);
    }
    QVERIFY2(duplicates.isEmpty(),
             qPrintable(QStringLiteral("duplicate filter id(s): %1")
                            .arg(duplicates.join(QStringLiteral(", ")))));
}

// The Python API dispatches on this name, so a collision makes one of the two
// filters unreachable from a script.
void FilterDescriptorTests::pythonNamesAreUnique()
{
    QHash<QString, QString> owner;
    QStringList collisions;
    for (const auto &info : m_infos) {
        const QString python = info.descriptor.effectivePythonName();
        const auto it = owner.constFind(python);
        if (it != owner.constEnd())
            collisions << QStringLiteral("%1: %2 and %3").arg(python, it.value(),
                                                              info.descriptor.id);
        else
            owner.insert(python, info.descriptor.id);
    }
    QVERIFY2(collisions.isEmpty(),
             qPrintable(QStringLiteral("colliding Python name(s):\n  %1")
                            .arg(collisions.join(QStringLiteral("\n  ")))));
}

void FilterDescriptorTests::descriptorConforms_data()
{
    QTest::addColumn<QString>("key");
    for (const auto &info : m_infos)
        QTest::newRow(qPrintable(info.descriptor.id)) << info.key;
}

void FilterDescriptorTests::descriptorConforms()
{
    QFETCH(QString, key);

    const auto it = std::find_if(m_infos.begin(), m_infos.end(),
                                 [&](const Document::FilterInfo &fi) { return fi.key == key; });
    QVERIFY2(it != m_infos.end(), qPrintable(QStringLiteral("no descriptor for key %1").arg(key)));
    const MeshFilterDescriptor &d = it->descriptor;

    // Every problem with this filter is collected before failing, so one run tells
    // you everything wrong with it rather than one thing per edit-build-run cycle.
    QStringList problems;
    checkIdentity(d, problems);
    checkClassification(d, problems);
    checkParameters(d, problems);
    checkCodes(d, problems);
    checkReferences(d, problems);

    QVERIFY2(problems.isEmpty(),
             qPrintable(QStringLiteral("%1: %2 problem(s):\n  %3")
                            .arg(d.id, QString::number(problems.size()),
                                 problems.join(QStringLiteral("\n  ")))));
}

void FilterDescriptorTests::checkIdentity(const MeshFilterDescriptor &d,
                                          QStringList &problems) const
{
    static const QRegularExpression identifierRe(QStringLiteral("^[a-z][a-z0-9_]*$"));

    if (!identifierRe.match(d.id).hasMatch())
        problems << QStringLiteral("id '%1' is not lower_snake_case").arg(d.id);
    if (d.name.trimmed().isEmpty())
        problems << QStringLiteral("empty display name");
    if (d.name != d.name.trimmed())
        problems << QStringLiteral("display name '%1' has surrounding whitespace").arg(d.name);
    if (d.shortDescription.trimmed().isEmpty())
        problems << QStringLiteral("empty shortDescription");

    const QString python = d.effectivePythonName();
    if (!identifierRe.match(python).hasMatch())
        problems << QStringLiteral("Python name '%1' is not lower_snake_case").arg(python);

    // The naming grammar of vocabulary.md section 6, enforced for the category roots
    // whose renaming round has landed. A filter carrying a second category in an
    // applied root while belonging to a pending one is judged by its primary.
    const QString name = d.name.trimmed();
    if (name.isEmpty() || d.categories.isEmpty())
        return;
    const QString root = d.categories.front().section(QLatin1Char('/'), 0, 0);
    if (!m_appliedRoots.contains(root))
        return;
    for (const QString &named : m_namedResults) {
        if (name.startsWith(named))
            return;
    }
    if (name.contains(QLatin1Char(':'))) {
        problems << QStringLiteral("display name '%1' repeats the category with a colon")
                        .arg(name);
        return;
    }
    const QString first = name.section(QLatin1Char(' '), 0, 0);
    if (!m_verbs.contains(first)) {
        problems << QStringLiteral("display name '%1' leads with '%2', which is not in the "
                                   "verb lexicon").arg(name, first);
    }
}

void FilterDescriptorTests::checkClassification(const MeshFilterDescriptor &d,
                                                QStringList &problems) const
{
    if (d.categories.isEmpty()) {
        problems << QStringLiteral("no categories");
        return;
    }
    for (const QString &c : d.categories) {
        if (!FilterCategories::isValid(c))
            problems << QStringLiteral("category '%1' is not in the ontology").arg(c);
    }

    // A descriptor declares an id; the plugin dispatches on that same string, held as
    // a literal in its own source. Nothing links the two, so a rename that updates one
    // and not the other leaves a filter that is listed, enabled, and refuses when
    // picked. Checked against the sources because on this document most filters would
    // never reach their plugin anyway.
    if (!m_pluginSources.contains(QStringLiteral("\"%1\"").arg(d.id)))
        problems << QStringLiteral("id appears in no plugin source, so nothing dispatches it");
}

void FilterDescriptorTests::checkParameters(const MeshFilterDescriptor &d,
                                            QStringList &problems) const
{
    QSet<QString> ids;
    for (const auto &p : d.parameters) {
        if (p.id.trimmed().isEmpty()) {
            problems << QStringLiteral("a parameter has an empty id");
            continue;
        }
        if (ids.contains(p.id))
            problems << QStringLiteral("parameter '%1' is declared twice").arg(p.id);
        ids.insert(p.id);

        if (p.label.trimmed().isEmpty())
            problems << QStringLiteral("parameter '%1' has no label").arg(p.id);
        if (p.helpMarkdown.trimmed().isEmpty())
            problems << QStringLiteral("parameter '%1' has no help text").arg(p.id);

        if (p.type == MeshFilterParameterType::Enum) {
            if (p.enumOptions.empty()) {
                problems << QStringLiteral("enum parameter '%1' declares no options").arg(p.id);
            } else {
                QSet<QString> optionIds;
                for (const auto &o : p.enumOptions) {
                    if (optionIds.contains(o.id)) {
                        problems << QStringLiteral("enum parameter '%1' repeats option '%2'")
                                        .arg(p.id, o.id);
                    }
                    optionIds.insert(o.id);
                    if (o.label.trimmed().isEmpty()) {
                        problems << QStringLiteral("enum parameter '%1' option '%2' has no label")
                                        .arg(p.id, o.id);
                    }
                }
                const QString def = p.defaultValue.toString();
                if (!optionIds.contains(def)) {
                    problems << QStringLiteral("enum parameter '%1' defaults to '%2', which is "
                                               "not one of its options").arg(p.id, def);
                }
            }
        }

        // Ranges are checked after token resolution, so a default that only violates
        // its bound once a mesh is present -- a fixed number against an "@bboxDiag"
        // range -- is caught here and not left for a user to discover.
        if (isNumericParameter(p.type)) {
            double minimum = 0.0;
            double maximum = 0.0;
            double value = 0.0;
            const bool hasMin = asBound(p.minValue, minimum);
            const bool hasMax = asBound(p.maxValue, maximum);
            const bool hasValue = asBound(p.defaultValue, value);
            if (hasMin && hasMax && minimum >= maximum) {
                problems << QStringLiteral("parameter '%1' has an empty range [%2, %3]")
                                .arg(p.id).arg(minimum).arg(maximum);
            }
            if (hasValue && hasMin && value < minimum) {
                problems << QStringLiteral("parameter '%1' defaults to %2, below its minimum %3")
                                .arg(p.id).arg(value).arg(minimum);
            }
            if (hasValue && hasMax && value > maximum) {
                problems << QStringLiteral("parameter '%1' defaults to %2, above its maximum %3")
                                .arg(p.id).arg(value).arg(maximum);
            }
        }
    }

    // enabledWhen names the bool parameter that gates the editor; a typo silently
    // leaves the control permanently enabled.
    for (const auto &p : d.parameters) {
        QString gate = p.enabledWhen;
        if (gate.startsWith(QLatin1Char('!')))
            gate = gate.mid(1);
        if (gate.isEmpty())
            continue;
        if (!ids.contains(gate)) {
            problems << QStringLiteral("parameter '%1' is gated on '%2', which it does not "
                                       "declare").arg(p.id, gate);
        }
    }

    // See docs/design/history/filter_names.md: a randomized filter offers a seed, and 0 means
    // "fresh every run". Which filters must have one is asserted elsewhere; this pins
    // the shape wherever the parameter appears.
    for (const auto &p : d.parameters) {
        if (p.id != QStringLiteral("randomSeed"))
            continue;
        if (p.type != MeshFilterParameterType::Int)
            problems << QStringLiteral("randomSeed is not an int");
        if (p.defaultValue.toInt() != 0)
            problems << QStringLiteral("randomSeed does not default to 0");
    }
}

void FilterDescriptorTests::checkCodes(const MeshFilterDescriptor &d,
                                       QStringList &problems) const
{
    // outputModifies says which attributes of the *current mesh* a filter rewrites, and
    // the framework reads it only for that: to refresh the polygon face-count cache when
    // FV/FP appear, and to take the cheap selection-delta undo path when nothing but
    // VS/FS does. On a filter that produces new layers instead there is no current mesh
    // being modified, so the codes describe nothing and the two consumers above would be
    // acting on a mesh the filter never touched. 28 filters had accumulated one anyway.
    if (!d.outputModifies.isEmpty()
        && d.outputDomain != MeshFilterOutputDomain::ModifyCurrentMesh) {
        problems << QStringLiteral("declares outputModifies (%1) but does not modify the "
                                   "current mesh")
                        .arg(d.outputModifies.join(QStringLiteral(", ")));
    }
    for (const QString &code : d.outputModifies) {
        if (!outputModifyCodes().contains(code))
            problems << QStringLiteral("unknown outputModifies code '%1'").arg(code);
    }
    for (const QString &code : d.inputPrepare) {
        if (!inputPrepareCodes().contains(code))
            problems << QStringLiteral("unknown inputPrepare code '%1'").arg(code);
    }
}

// The reference machinery (markdownCitation / bibTeX / doiUrl) feeds the in-app help
// and the generated documentation's bibliography, both of which render whatever is
// here without validating it.
void FilterDescriptorTests::checkReferences(const MeshFilterDescriptor &d,
                                            QStringList &problems) const
{
    for (const auto &r : d.references) {
        const QString label = r.id.isEmpty() ? QStringLiteral("<no id>") : r.id;
        if (r.id.isEmpty())
            problems << QStringLiteral("a reference has no citation id");
        if (r.title.trimmed().isEmpty())
            problems << QStringLiteral("reference '%1' has no title").arg(label);
        if (r.year <= 0)
            problems << QStringLiteral("reference '%1' has no year").arg(label);
        if (r.bibTeX().trimmed().isEmpty())
            problems << QStringLiteral("reference '%1' renders an empty BibTeX entry").arg(label);
    }
}

QTEST_MAIN(FilterDescriptorTests)
#include "test_filter_descriptors.moc"
