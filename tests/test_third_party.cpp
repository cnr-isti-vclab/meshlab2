// The About dialog's Libraries tab is the only place the user is told what geometry code
// MeshLab is built on, and nothing generates it: the two lists are plain C++ arrays in
// src/ui/aboutdialog.cpp. A list nobody regenerates goes stale silently, so this reads the
// arrays back out of the source and checks them against what the build actually pulls in.
//
// The ledger below is the point of the suite, not a suppression file. Every submodule,
// every vcpkg dependency and every vendored tree must be either named in the About lists
// or listed here with a reason, and every About entry must correspond to something that
// really is pulled in -- so the check fails in both directions and cannot rot quietly.
//
// See docs/design/architecture.md, "Third-party geometry code".
#include <QFile>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QTest>

namespace {

QString sourceDir()
{
    return QStringLiteral(TEST_SOURCE_DIR);
}

QString readFile(const QString &relativePath)
{
    QFile file(sourceDir() + QLatin1Char('/') + relativePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    return QString::fromUtf8(file.readAll());
}

// What each thing the build pulls in is called in the About dialog. The key is what the
// build system says: a submodule path, a vcpkg dependency, or the directory a tree is
// vendored into.
const QHash<QString, QString> &aboutName()
{
    static const QHash<QString, QString> table = {
        // Submodules (.gitmodules)
        { QStringLiteral("vcglib"),                       QStringLiteral("VCGLib") },
        { QStringLiteral("external/trueform"),            QStringLiteral("TrueForm") },
        { QStringLiteral("external/meshfix"),             QStringLiteral("MeshFix") },
        { QStringLiteral("external/qslim"),               QStringLiteral("QSlim") },
        { QStringLiteral("external/quadwild-bimdf"),      QStringLiteral("QuadWild-BiMDF") },
        // vcpkg dependencies (vcpkg.json)
        { QStringLiteral("cgal"),                         QStringLiteral("CGAL") },
        { QStringLiteral("libigl"),                       QStringLiteral("libigl") },
        { QStringLiteral("geogram"),                      QStringLiteral("Geogram") },
        { QStringLiteral("embree"),                       QStringLiteral("Embree") },
        // Vendored trees (plugins/*/upstream, plugins/*/Src)
        { QStringLiteral("filter_bpa"),                   QStringLiteral("BPA") },
        { QStringLiteral("filter_instant_meshes"),        QStringLiteral("Instant Meshes") },
        { QStringLiteral("filter_isoparam"),              QStringLiteral("Isoparametrization") },
        { QStringLiteral("filter_screened_poisson"),      QStringLiteral("PoissonRecon") },
        { QStringLiteral("filter_texture_defragmentation"),
                                                          QStringLiteral("Texture Defragmentation") },
        { QStringLiteral("filter_xatlas"),                QStringLiteral("xatlas") },
    };
    return table;
}

// Pulled in, but not geometry processing, so deliberately absent from the tab. The tab
// says as much; these are the specifics, so that a newly added dependency has to be
// classified by a person rather than slipping through unnoticed.
const QHash<QString, QString> &notGeometry()
{
    static const QHash<QString, QString> table = {
        { QStringLiteral("external/jkqtplotter"), QStringLiteral("renders formulas in filter help") },
        { QStringLiteral("draco"),          QStringLiteral("mesh compression codec, for glTF I/O") },
        { QStringLiteral("nanobind"),       QStringLiteral("Python bindings") },
        { QStringLiteral("tinygltf"),       QStringLiteral("glTF file format") },
        { QStringLiteral("lib3mf"),         QStringLiteral("3MF file format") },
        { QStringLiteral("libe57format"),   QStringLiteral("E57 file format") },
        { QStringLiteral("xerces-c"),       QStringLiteral("XML, required by libE57Format") },
        { QStringLiteral("rapidobj"),       QStringLiteral("OBJ file format") },
        { QStringLiteral("stb"),            QStringLiteral("image loading and writing") },
        { QStringLiteral("muparser"),       QStringLiteral("expression evaluation for the expression filters") },
        { QStringLiteral("glm"),            QStringLiteral("vector and matrix types") },
        { QStringLiteral("tbb"),            QStringLiteral("threading, used through other libraries") },
    };
    return table;
}

// The entries of one array in aboutdialog.cpp: every { "Name", "https://...", ... } in it.
QStringList aboutEntryNames(const QString &source, const QString &arrayName, QString *error)
{
    const int declaration = source.indexOf(arrayName);
    if (declaration < 0) {
        *error = QStringLiteral("no array named %1 in src/ui/aboutdialog.cpp").arg(arrayName);
        return {};
    }
    const int open = source.indexOf(QLatin1Char('{'), declaration);
    if (open < 0) {
        *error = QStringLiteral("%1 has no initializer").arg(arrayName);
        return {};
    }
    int depth = 0;
    int close = -1;
    for (int i = open; i < source.size(); ++i) {
        if (source.at(i) == QLatin1Char('{'))
            ++depth;
        else if (source.at(i) == QLatin1Char('}') && --depth == 0) {
            close = i;
            break;
        }
    }
    if (close < 0) {
        *error = QStringLiteral("%1 initializer is not closed").arg(arrayName);
        return {};
    }

    static const QRegularExpression entryRe(
        QStringLiteral("\\{\\s*\"([^\"]+)\"\\s*,\\s*\"(https://[^\"]+)\""));
    QStringList names;
    auto it = entryRe.globalMatch(source.mid(open, close - open));
    while (it.hasNext())
        names << it.next().captured(1);
    return names;
}

} // namespace

class ThirdPartyTests : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void aboutListsAreWellFormed();
    void everySubmoduleIsAccountedFor();
    void everyPackageDependencyIsAccountedFor();
    void everyVendoredTreeIsAccountedFor();
    void everyAboutEntryComesFromSomething();

private:
    QStringList m_listed;      // names in the two About arrays
    QStringList m_libraries;
    QStringList m_packages;
};

void ThirdPartyTests::initTestCase()
{
    const QString about = readFile(QStringLiteral("src/ui/aboutdialog.cpp"));
    QVERIFY2(!about.isEmpty(), "could not read src/ui/aboutdialog.cpp");

    QString error;
    m_libraries = aboutEntryNames(about, QStringLiteral("kGeometryLibraries[]"), &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    m_packages = aboutEntryNames(about, QStringLiteral("kFocusedPackages[]"), &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    m_listed = m_libraries + m_packages;
}

void ThirdPartyTests::aboutListsAreWellFormed()
{
    QVERIFY2(m_libraries.size() >= 4,
             qPrintable(QStringLiteral("only %1 geometry libraries listed").arg(m_libraries.size())));
    QVERIFY2(m_packages.size() >= 5,
             qPrintable(QStringLiteral("only %1 vendored packages listed").arg(m_packages.size())));

    QStringList duplicates;
    QSet<QString> seen;
    for (const QString &name : std::as_const(m_listed)) {
        if (seen.contains(name))
            duplicates << name;
        seen.insert(name);
    }
    QVERIFY2(duplicates.isEmpty(),
             qPrintable(QStringLiteral("listed twice: %1").arg(duplicates.join(QStringLiteral(", ")))));
}

// The shared body of the three source-side checks: everything the build pulls in has to be
// classified, and the name it claims has to be one the About dialog really shows.
static void checkAccountedFor(const QStringList &keys, const QStringList &listed,
                              const QString &what)
{
    QStringList unclassified;
    QStringList missing;
    for (const QString &key : keys) {
        const auto named = aboutName().constFind(key);
        if (named == aboutName().constEnd()) {
            if (!notGeometry().contains(key))
                unclassified << key;
            continue;
        }
        if (!listed.contains(named.value()))
            missing << QStringLiteral("%1 (expected '%2')").arg(key, named.value());
    }

    QVERIFY2(unclassified.isEmpty(),
             qPrintable(QStringLiteral(
                 "%1 not classified: %2. Add each to the About dialog's Libraries tab and to "
                 "aboutName() in this file, or to notGeometry() with a reason.")
                            .arg(what, unclassified.join(QStringLiteral(", ")))));
    QVERIFY2(missing.isEmpty(),
             qPrintable(QStringLiteral(
                 "%1 missing from the About dialog's Libraries tab: %2")
                            .arg(what, missing.join(QStringLiteral("; ")))));
}

void ThirdPartyTests::everySubmoduleIsAccountedFor()
{
    const QString gitmodules = readFile(QStringLiteral(".gitmodules"));
    QVERIFY2(!gitmodules.isEmpty(), "could not read .gitmodules");

    static const QRegularExpression pathRe(QStringLiteral("^\\s*path\\s*=\\s*(\\S+)"),
                                           QRegularExpression::MultilineOption);
    QStringList paths;
    auto it = pathRe.globalMatch(gitmodules);
    while (it.hasNext())
        paths << it.next().captured(1);

    QVERIFY2(paths.size() >= 5,
             qPrintable(QStringLiteral("only %1 submodules found").arg(paths.size())));
    checkAccountedFor(paths, m_listed, QStringLiteral("submodules"));
}

void ThirdPartyTests::everyPackageDependencyIsAccountedFor()
{
    const QString manifest = readFile(QStringLiteral("vcpkg.json"));
    QVERIFY2(!manifest.isEmpty(), "could not read vcpkg.json");

    const QJsonDocument doc = QJsonDocument::fromJson(manifest.toUtf8());
    QVERIFY2(doc.isObject(), "vcpkg.json is not a JSON object");
    const QJsonArray dependencies = doc.object().value(QStringLiteral("dependencies")).toArray();

    QStringList names;
    for (const QJsonValue &value : dependencies) {
        // A dependency is a bare string, or an object when it carries features.
        names << (value.isString() ? value.toString()
                                   : value.toObject().value(QStringLiteral("name")).toString());
    }

    QVERIFY2(names.size() >= 10,
             qPrintable(QStringLiteral("only %1 dependencies found").arg(names.size())));
    checkAccountedFor(names, m_listed, QStringLiteral("vcpkg dependencies"));
}

void ThirdPartyTests::everyVendoredTreeIsAccountedFor()
{
    // A vendored upstream lives in a directory of its own beside the plugin that uses it.
    // "Src" is PoissonRecon's own spelling, kept so its tree matches upstream exactly.
    QDir plugins(sourceDir() + QStringLiteral("/plugins"));
    QStringList vendored;
    const QStringList pluginDirs = plugins.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &plugin : pluginDirs) {
        for (const QString &candidate : { QStringLiteral("upstream"), QStringLiteral("Src") }) {
            if (QDir(plugins.filePath(plugin + QLatin1Char('/') + candidate)).exists()) {
                vendored << plugin;
                break;
            }
        }
    }

    QVERIFY2(vendored.size() >= 5,
             qPrintable(QStringLiteral("only %1 vendored trees found: %2")
                            .arg(vendored.size()).arg(vendored.join(QStringLiteral(", ")))));
    checkAccountedFor(vendored, m_listed, QStringLiteral("vendored trees"));
}

// The other direction: a library dropped from the build must not linger in the dialog.
void ThirdPartyTests::everyAboutEntryComesFromSomething()
{
    QSet<QString> reachable;
    for (auto it = aboutName().constBegin(); it != aboutName().constEnd(); ++it)
        reachable.insert(it.value());

    QStringList orphans;
    for (const QString &name : std::as_const(m_listed)) {
        if (!reachable.contains(name))
            orphans << name;
    }
    QVERIFY2(orphans.isEmpty(),
             qPrintable(QStringLiteral(
                 "listed in the About dialog but not produced by any submodule, vcpkg "
                 "dependency or vendored tree: %1. Either it is no longer used and the "
                 "entry should go, or aboutName() in this file needs to learn where it "
                 "comes from.")
                            .arg(orphans.join(QStringLiteral(", ")))));
}

QTEST_MAIN(ThirdPartyTests)
#include "test_third_party.moc"
