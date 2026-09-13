#include <QtTest/QtTest>
#include <QDirIterator>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QTextStream>

#include "document.h"
#include "filtercategories.h"

// Tests every filter that, according to its manifest descriptor, takes no mesh
// as input (inputDomain == None) and produces new meshes (outputDomain ==
// NewMeshes).  Each such filter is run with its default parameter values.
//
// The data-driven pattern gives every filter its own named row in the Qt Test
// output.  Results are also collected manually and written as an HTML report at
// the end of the test run (see cleanupTestCase).
//
// Report path: MESHLAB2_REPORT_FILE env var, or filter_creation_report.html in
// the current working directory.

// ---------------------------------------------------------------------------
// Result record (one per filter row)
// ---------------------------------------------------------------------------

struct FilterTestResult {
    QString filterId;
    QString filterName;
    bool    passed        = false;
    QString failReason;
    int     newMeshCount  = 0;
    int     totalVertices = 0;
};

// ---------------------------------------------------------------------------
// HTML helpers
// ---------------------------------------------------------------------------

static QString htmlEscape(const QString &s)
{
    QString out = s;
    out.replace(QLatin1Char('&'),  QStringLiteral("&amp;"));
    out.replace(QLatin1Char('<'),  QStringLiteral("&lt;"));
    out.replace(QLatin1Char('>'),  QStringLiteral("&gt;"));
    out.replace(QLatin1Char('"'),  QStringLiteral("&quot;"));
    return out;
}

static void writeHtmlReport(const QVector<FilterTestResult> &results, const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        qWarning() << "Could not write HTML report to" << path;
        return;
    }

    const int total  = results.size();
    const int passed = static_cast<int>(
        std::count_if(results.begin(), results.end(),
                      [](const FilterTestResult &r) { return r.passed; }));
    const int failed = total - passed;
    const QString timestamp = QDateTime::currentDateTime().toString(Qt::ISODate);
    const bool allPass = failed == 0;

    QTextStream o(&file);
    o.setEncoding(QStringConverter::Utf8);

    // ---- page head --------------------------------------------------------
    o << R"(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>Filter Creation Test Report — QMeshLab</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;background:#f4f6f9;color:#222}
.hdr{background:#1e2a3a;color:#fff;padding:1.4rem 2rem}
.hdr h1{font-size:1.35rem;font-weight:600}
.hdr .meta{font-size:.82rem;color:#9ab;margin-top:.3rem}
.summary{display:flex;align-items:center;gap:1.2rem;padding:1rem 2rem;background:#fff;border-bottom:1px solid #dde;flex-wrap:wrap}
.badge{padding:.35rem 1rem;border-radius:4px;font-weight:700;font-size:.88rem}
.all-pass{background:#d4edda;color:#155724}
.some-fail{background:#f8d7da;color:#721c24}
.stat{font-size:.9rem}
.stat b{font-weight:700}
.pv{color:#28a745}.fv{color:#dc3545}
.content{padding:1.4rem 2rem}
table{width:100%;border-collapse:collapse;background:#fff;border-radius:6px;overflow:hidden;box-shadow:0 1px 4px rgba(0,0,0,.1)}
th{background:#1e2a3a;color:#fff;text-align:left;padding:.65rem 1rem;font-size:.78rem;font-weight:500;letter-spacing:.05em;text-transform:uppercase}
td{padding:.65rem 1rem;border-bottom:1px solid #eef;font-size:.88rem;vertical-align:top}
tr:last-child td{border-bottom:none}
tr:hover td{background:#f8fafc}
.rpass{color:#28a745;font-weight:700}
.rfail{color:#dc3545;font-weight:700}
.fd{color:#999;font-size:.8rem;margin-top:.2rem}
code{font-family:"SF Mono",Consolas,monospace;font-size:.83em;background:#f0f2f5;padding:.1em .4em;border-radius:3px}
.num{text-align:right}
</style>
</head>
<body>
)";

    // ---- header -----------------------------------------------------------
    o << "<div class=\"hdr\">\n"
      << "  <h1>Filter Creation Test Report &mdash; QMeshLab</h1>\n"
      << "  <div class=\"meta\">Generated: " << htmlEscape(timestamp) << "</div>\n"
      << "</div>\n";

    // ---- summary bar ------------------------------------------------------
    o << "<div class=\"summary\">\n"
      << "  <span class=\"badge " << (allPass ? "all-pass" : "some-fail") << "\">"
      << (allPass ? "ALL PASSED" : "FAILURES") << "</span>\n"
      << "  <div class=\"stat\">Total&nbsp;<b>" << total << "</b></div>\n"
      << "  <div class=\"stat\">Passed&nbsp;<b class=\"pv\">" << passed << "</b></div>\n"
      << "  <div class=\"stat\">Failed&nbsp;<b class=\"fv\">" << failed << "</b></div>\n"
      << "</div>\n";

    // ---- table ------------------------------------------------------------
    o << "<div class=\"content\">\n"
      << "<table>\n<thead>\n"
      << "<tr><th>#</th><th>Filter ID</th><th>Name</th>"
         "<th>Result</th><th class=\"num\">New meshes</th>"
         "<th class=\"num\">Vertices</th></tr>\n"
      << "</thead>\n<tbody>\n";

    for (int i = 0; i < results.size(); ++i) {
        const FilterTestResult &r = results[i];
        o << "<tr>\n"
          << "  <td>" << (i + 1) << "</td>\n"
          << "  <td><code>" << htmlEscape(r.filterId) << "</code></td>\n"
          << "  <td>" << htmlEscape(r.filterName) << "</td>\n";
        if (r.passed) {
            o << "  <td><span class=\"rpass\">&#x2713; PASS</span></td>\n"
              << "  <td class=\"num\">" << r.newMeshCount << "</td>\n"
              << "  <td class=\"num\">" << r.totalVertices << "</td>\n";
        } else {
            o << "  <td><span class=\"rfail\">&#x2717; FAIL</span>"
              << "<div class=\"fd\">" << htmlEscape(r.failReason) << "</div></td>\n"
              << "  <td class=\"num\">&mdash;</td>\n"
              << "  <td class=\"num\">&mdash;</td>\n";
        }
        o << "</tr>\n";
    }

    o << "</tbody>\n</table>\n</div>\n</body>\n</html>\n";
}

// ---------------------------------------------------------------------------
// Test class
// ---------------------------------------------------------------------------

class FilterCreationTests : public QObject
{
    Q_OBJECT

private:
    QVector<FilterTestResult> m_results;

private slots:
    void ontologyIsWellFormed();
    void everyFilterIsClassified();
    void everyDeclaredFilterIsDispatchable();
    void runWithDefaults_data();
    void runWithDefaults();
    void cleanupTestCase();
};

// The ontology API itself: see docs/design/vocabulary.md §1.
void FilterCreationTests::ontologyIsWellFormed()
{
    const QStringList roots = FilterCategories::roots();
    QVERIFY2(!roots.isEmpty(), "ontology must declare roots");

    for (const QString &root : roots) {
        QVERIFY2(FilterCategories::isValid(root), qPrintable(root));
        for (const QString &sub : FilterCategories::subcategories(root))
            QVERIFY2(FilterCategories::isValid(root + QLatin1Char('/') + sub),
                     qPrintable(root + QLatin1Char('/') + sub));
    }

    // Rejections: unknown root, unknown subcategory, a third level (the ontology is
    // two levels by construction), and empty.
    QVERIFY(!FilterCategories::isValid(QStringLiteral("Nonexistent")));
    QVERIFY(!FilterCategories::isValid(QStringLiteral("Meshing/Nonexistent")));
    QVERIFY(!FilterCategories::isValid(QStringLiteral("Meshing/Simplification/Extra")));
    QVERIFY(!FilterCategories::isValid(QString()));

    // Surrounding whitespace is tolerated; spelling is not negotiable.
    QVERIFY(FilterCategories::isValid(QStringLiteral(" Meshing / Simplification ")));
    QVERIFY(!FilterCategories::isValid(QStringLiteral("meshing/simplification")));
}

// Guards the classification against drift: this is what stops a new filter
// inventing a category, which is how the previous 32 free-text menuPath values
// accumulated. See docs/design/history/filter_classification.md.
void FilterCreationTests::everyFilterIsClassified()
{
    Document doc;
    const std::vector<Document::FilterInfo> infos = doc.filterInfos();
    QVERIFY2(!infos.empty(), "Filter registry must be non-empty");

    QStringList problems;
    for (const auto &info : infos) {
        const MeshFilterDescriptor &d = info.descriptor;
        if (d.categories.isEmpty()) {
            problems << QStringLiteral("%1: no categories").arg(d.id);
            continue;
        }
        QStringList seen;
        for (const QString &c : d.categories) {
            if (!FilterCategories::isValid(c))
                problems << QStringLiteral("%1: '%2' is not in the ontology").arg(d.id, c);
            if (seen.contains(c))
                problems << QStringLiteral("%1: '%2' listed twice").arg(d.id, c);
            seen << c;
        }
    }
    if (!problems.isEmpty())
        QFAIL(qPrintable(QStringLiteral("%1 classification problem(s):\n  %2")
                             .arg(problems.size())
                             .arg(problems.join(QStringLiteral("\n  ")))));
}

// Populate one row per eligible filter.  The row tag (= filter id) is used by
// Qt Test as the sub-test name in output and in --testcase selectors.
// A descriptor declares an id; the plugin dispatches on that same string, held as a
// literal in its own source. Nothing links the two, so a rename that updates one and not
// the other leaves a filter that is listed, enabled, and refuses when picked.
//
// This is checked against the sources rather than by running the filters: the manager
// validates the input domain before it dispatches, so on an empty document no plugin ever
// sees the id, and running them on a real one means actually running three hundred
// filters. Pass 2 renamed 281 ids at once, which is when this stopped being hypothetical.
void FilterCreationTests::everyDeclaredFilterIsDispatchable()
{
    QString sources;
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
            sources += QString::fromUtf8(f.readAll());
    }
    QVERIFY2(sources.size() > 100000, "could not read the plugin sources");

    Document doc;
    QStringList undispatchable;
    for (const auto &info : doc.filterInfos()) {
        if (!sources.contains(QStringLiteral("\"%1\"").arg(info.descriptor.id)))
            undispatchable << QStringLiteral("%1 (%2)").arg(info.descriptor.name,
                                                            info.descriptor.id);
    }
    QVERIFY2(undispatchable.isEmpty(),
             qPrintable(QStringLiteral("%1 filter id(s) appear in no plugin source, so no "
                                       "plugin will dispatch them:\n  %2")
                            .arg(undispatchable.size())
                            .arg(undispatchable.join(QStringLiteral("\n  ")))));
}

void FilterCreationTests::runWithDefaults_data()
{
    QTest::addColumn<QString>("key");
    QTest::addColumn<QString>("filterName");

    Document doc;
    const std::vector<Document::FilterInfo> infos = doc.filterInfos();
    QVERIFY2(!infos.empty(), "Filter registry must be non-empty");

    for (const auto &info : infos) {
        if (info.descriptor.inputDomain != MeshFilterInputDomain::None)
            continue;
        if (info.descriptor.outputDomain != MeshFilterOutputDomain::NewMeshes)
            continue;
        QTest::newRow(qPrintable(info.descriptor.id))
            << info.key
            << info.descriptor.name;
    }
}

void FilterCreationTests::runWithDefaults()
{
    QFETCH(QString, key);
    QFETCH(QString, filterName);

    // Always populate a record before any QFAIL/QVERIFY so cleanupTestCase
    // can include it in the report regardless of how the test exits.
    FilterTestResult record;
    record.filterId   = QTest::currentDataTag();
    record.filterName = filterName;

    Document doc;
    const std::vector<Document::FilterInfo> infos = doc.filterInfos();
    const auto it = std::find_if(infos.begin(), infos.end(),
                                 [&](const Document::FilterInfo &fi) { return fi.key == key; });

    if (it == infos.end()) {
        record.failReason = "Filter key not found in registry: " + key;
        m_results.append(record);
        QFAIL(qPrintable(record.failReason));
    }

    if (!it->applicable) {
        record.failReason = "Not applicable with no meshes loaded: " + it->applicabilityError;
        m_results.append(record);
        QFAIL(qPrintable(record.failReason));
    }

    const int meshCountBefore = doc.meshCount();
    const MeshFilterRunResult result = doc.runFilter(key, {});

    if (!result.success) {
        record.failReason = result.errorMessage;
        m_results.append(record);
        QFAIL(qPrintable(result.errorMessage));
    }

    if (result.newMeshIndices.isEmpty()) {
        record.failReason = QStringLiteral("Run succeeded but created no new meshes");
        m_results.append(record);
        QFAIL(qPrintable(record.failReason));
    }

    int totalVerts = 0;
    for (int idx : result.newMeshIndices) {
        if (idx >= 0 && idx < doc.meshCount())
            totalVerts += doc.mesh(idx).mesh.VN();
    }

    if (totalVerts <= 0) {
        record.failReason = QStringLiteral("New mesh(es) have no vertices");
        m_results.append(record);
        QFAIL(qPrintable(record.failReason));
    }

    record.passed        = true;
    record.newMeshCount  = result.newMeshIndices.size();
    record.totalVertices = totalVerts;
    m_results.append(record);

    QVERIFY(doc.meshCount() > meshCountBefore);
}

void FilterCreationTests::cleanupTestCase()
{
    const QString envPath = qEnvironmentVariable("MESHLAB2_REPORT_FILE");
    QString path;
    if (!envPath.isEmpty()) {
        path = envPath;
    } else {
        const QString reportsDir =
            QStringLiteral(TEST_SOURCE_DIR "/tests/reports");
        QDir().mkpath(reportsDir);
        path = reportsDir + QStringLiteral("/filter_creation_report.html");
    }

    writeHtmlReport(m_results, path);
    qInfo() << "HTML report written to:" << path;
}

QTEST_MAIN(FilterCreationTests)
#include "test_filter_creation.moc"
