#include "aboutdialog.h"

#include <QClipboard>
#include <QDialogButtonBox>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPixmap>
#include <QPushButton>
#include <QSysInfo>
#include <QTabWidget>
#include <QTextBrowser>
#include <QVBoxLayout>
#include <iterator>

namespace {

QString compilerName()
{
#if defined(__clang__)
    return QStringLiteral("Clang %1").arg(QString::fromLatin1(__clang_version__));
#elif defined(_MSC_VER)
    return QStringLiteral("MSVC %1").arg(_MSC_VER);
#elif defined(__GNUC__)
    return QStringLiteral("GCC %1.%2.%3")
        .arg(__GNUC__).arg(__GNUC_MINOR__).arg(__GNUC_PATCHLEVEL__);
#else
    return QObject::tr("Unknown compiler");
#endif
}

QString buildMode()
{
#ifdef QT_DEBUG
    return QObject::tr("Debug");
#else
    return QObject::tr("Release");
#endif
}

QString buildId()
{
#ifdef MESHLAB2_BUILD_ID
    return QStringLiteral(MESHLAB2_BUILD_ID);
#else
    return QObject::tr("Unavailable");
#endif
}

QString buildInformation()
{
    return QObject::tr(
        "MeshLab build: %1\n"
        "Built: %2\n"
        "Configuration: %3\n"
        "Qt: %4 (built with %5)\n"
        "Compiler: %6\n"
        "Operating system: %7\n"
        "CPU architecture: %8")
        .arg(buildId(),
             QStringLiteral(__DATE__),
             buildMode(),
             QString::fromLatin1(qVersion()),
             QStringLiteral(QT_VERSION_STR),
             compilerName(),
             QSysInfo::prettyProductName(),
             QSysInfo::currentCpuArchitecture());
}

// The geometry code MeshLab is built on, split by how much of it there is: a general
// library that many filters draw on, against a package vendored for one algorithm.
//
// These two lists are the only place either is written down for the user, so they have to
// be kept in step with what the build actually links -- the reminder, and where to look
// for what changed, is in docs/design/architecture.md.
struct ThirdPartyEntry
{
    const char *name;
    const char *url;
    const char *role;   // QT_TR_NOOP: translated where the table is rendered
};

const ThirdPartyEntry kGeometryLibraries[] = {
    { "VCGLib", "https://github.com/cnr-isti-vclab/vcglib",
      QT_TR_NOOP("The mesh data structure and the bulk of the processing: cleaning, "
                 "simplification, sampling, curvature, smoothing, reconstruction.") },
    { "CGAL", "https://www.cgal.org",
      QT_TR_NOOP("Exact-predicate geometry: alpha wrapping and alpha shapes, Poisson and "
                 "advancing-front reconstruction, optimal bounding boxes.") },
    { "libigl", "https://libigl.github.io",
      QT_TR_NOOP("Booleans, conformal and ARAP parametrization, curvature, exact and heat "
                 "geodesics, winding numbers.") },
    { "Geogram", "https://github.com/BrunoLevy/geogram",
      QT_TR_NOOP("Booleans, LSCM and spectral parametrization, chart segmentation and "
                 "packing, centroidal Voronoi remeshing.") },
    { "Embree", "https://www.embree.org",
      QT_TR_NOOP("Ray tracing behind ambient occlusion, obscurance, the shape diameter "
                 "function and visibility tests.") },
    { "TrueForm", "https://github.com/polydera/trueform",
      QT_TR_NOOP("Booleans, remeshing, signed distance, isocontours and a second OBJ/STL "
                 "reader. Separately licensed — see the license note below.") },
};

const ThirdPartyEntry kFocusedPackages[] = {
    { "PoissonRecon", "https://github.com/mkazhdan/PoissonRecon",
      QT_TR_NOOP("Screened Poisson and SSD surface reconstruction.") },
    { "MeshFix", "https://github.com/MarcoAttene/MeshFix-V2.1",
      QT_TR_NOOP("Turning a defective mesh into a single watertight manifold.") },
    { "QSlim", "https://github.com/alecjacobson/qslim",
      QT_TR_NOOP("Garland's original quadric edge-collapse simplification.") },
    { "Instant Meshes", "https://github.com/wjakob/instant-meshes",
      QT_TR_NOOP("Field-aligned quad remeshing.") },
    { "QuadWild-BiMDF", "https://github.com/cgg-bern/quadwild-bimdf",
      QT_TR_NOOP("Quad remeshing through a binary mixed-integer formulation.") },
    { "Texture Defragmentation", "https://github.com/maggio-a/texture-defrag",
      QT_TR_NOOP("Merging and defragmenting the charts of a texture atlas.") },
    { "xatlas", "https://github.com/jpcy/xatlas",
      QT_TR_NOOP("Automatic UV atlas generation and packing.") },
    { "BPA", "https://github.com/bernhardmgruber/bpa",
      QT_TR_NOOP("Gruber's ball-pivoting surface reconstruction.") },
    { "Isoparametrization", "https://github.com/cnr-isti-vclab/meshlab",
      QT_TR_NOOP("Abstract-domain parametrization and remeshing, from the original "
                 "MeshLab.") },
};

QString thirdPartyList(const ThirdPartyEntry *entries, std::size_t count)
{
    QString html = QStringLiteral("<table cellspacing='6'>");
    for (std::size_t i = 0; i < count; ++i) {
        html += QStringLiteral("<tr><td valign='top'><a href='%1'>%2</a></td>"
                               "<td valign='top'>%3</td></tr>")
                    .arg(QString::fromLatin1(entries[i].url),
                         QString::fromLatin1(entries[i].name).toHtmlEscaped(),
                         QObject::tr(entries[i].role).toHtmlEscaped());
    }
    return html + QStringLiteral("</table>");
}

QTextBrowser *htmlPage(const QString &html, QWidget *parent)
{
    auto *browser = new QTextBrowser(parent);
    browser->setOpenExternalLinks(true);
    browser->setFrameShape(QFrame::NoFrame);
    browser->setHtml(html);
    return browser;
}

QString htmlTableRow(const QString &label, const QString &value)
{
    return QStringLiteral("<tr><td><b>%1</b></td><td>%2</td></tr>")
        .arg(label.toHtmlEscaped(), value.toHtmlEscaped());
}

} // namespace

AboutDialog::AboutDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("About MeshLab"));
    setModal(true);
    resize(760, 540);
    setMinimumSize(620, 440);

    auto *root = new QVBoxLayout(this);
    auto *header = new QHBoxLayout;
    auto *logo = new QLabel(this);
    const QPixmap pixmap(QStringLiteral(":/img/MeshLab_Icon_512x512.png"));
    logo->setPixmap(pixmap.scaled(
        112, 112, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    logo->setFixedSize(120, 120);
    logo->setAlignment(Qt::AlignCenter);
    header->addWidget(logo);

    auto *identity = new QLabel(this);
    identity->setTextFormat(Qt::RichText);
    identity->setText(tr(
        "<div style='font-size:28pt; font-weight:600'>MeshLab</div>"
        "<div style='font-size:13pt'>The MeshLab rewrite for Qt 6</div>"
        "<div style='margin-top:8px; color:gray'>Build %1</div>")
        .arg(buildId().toHtmlEscaped()));
    identity->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
    header->addWidget(identity, 1);
    root->addLayout(header);

    auto *tabs = new QTabWidget(this);
    tabs->addTab(htmlPage(tr(
        "<h2>Open-source mesh processing</h2>"
        "<p>This is a from-scratch Qt 6 rewrite of MeshLab, the widely used "
        "system for editing, cleaning, repairing, inspecting, rendering, "
        "texturing and converting 3D meshes.</p>"
        "<p>It combines a modern Qt/QRhi application framework with "
        "<a href='https://github.com/cnr-isti-vclab/vcglib'>VCGLib</a> for mesh "
        "processing. It also aims to preserve geometry-processing "
        "algorithms as clearly attributed, reproducible plugins whose upstream "
        "implementations remain easy to update.</p>"
        "<h3>Project links</h3>"
        "<p><a href='https://github.com/cnr-isti-vclab/meshlab2'>Source code</a> &nbsp;·&nbsp; "
        "<a href='https://github.com/cnr-isti-vclab/meshlab2/issues'>Report an issue</a> &nbsp;·&nbsp; "
        "<a href='https://github.com/cnr-isti-vclab/meshlab2/tree/main/docs'>Documentation</a> &nbsp;·&nbsp; "
        "<a href='https://www.meshlab.net'>Original MeshLab</a></p>"), tabs),
        tr("Overview"));

    tabs->addTab(htmlPage(tr(
        "<h2>MeshLab lineage</h2>"
        "<p>It continues the MeshLab project developed at the "
        "<a href='https://vcg.isti.cnr.it'>Visual Computing "
        "Lab</a>, ISTI-CNR. Its architecture and interface are new, while its "
        "purpose, data model traditions and VCGLib foundation come from the "
        "original project.</p>"
        "<p>See the <a href='https://github.com/cnr-isti-vclab/meshlab2/graphs/contributors'>"
        "contributors to this rewrite</a> and the "
        "<a href='https://github.com/cnr-isti-vclab/meshlab/graphs/contributors'>"
        "contributors to the original MeshLab</a>.</p>"
        "<h2>Citing MeshLab and its algorithms</h2>"
        "<p>If MeshLab contributes to published work, please cite the relevant "
        "MeshLab publication and each processing algorithm used. Filters expose "
        "<b>[bib]</b>, <b>[doi]</b> and <b>[web]</b> links whenever their provenance "
        "metadata is available.</p>"
        "<p>The MeshLab publication list and suggested references are available at "
        "<a href='https://www.meshlab.net/#references'>meshlab.net/references</a>.</p>"
        "<h3>Contact</h3>"
        "<p>Please use the <a href='https://github.com/cnr-isti-vclab/meshlab2/issues'>"
        "issue tracker</a> for reproducible bugs and concrete feature requests.</p>"), tabs),
        tr("Credits and citations"));

    QString buildHtml = QStringLiteral("<h2>%1</h2><table cellspacing='7'>")
        .arg(tr("Build information").toHtmlEscaped());
    buildHtml += htmlTableRow(tr("Revision"), buildId());
    buildHtml += htmlTableRow(tr("Built"), QStringLiteral(__DATE__));
    buildHtml += htmlTableRow(tr("Configuration"), buildMode());
    buildHtml += htmlTableRow(
        tr("Qt"), tr("%1 (built with %2)").arg(
            QString::fromLatin1(qVersion()), QStringLiteral(QT_VERSION_STR)));
    buildHtml += htmlTableRow(tr("Compiler"), compilerName());
    buildHtml += htmlTableRow(tr("Operating system"), QSysInfo::prettyProductName());
    buildHtml += htmlTableRow(tr("CPU architecture"), QSysInfo::currentCpuArchitecture());
    buildHtml += tr(
        "</table><h2>License</h2>"
        "<p>MeshLab is free software distributed under the "
        "<a href='https://github.com/cnr-isti-vclab/meshlab2/blob/main/LICENSE'>"
        "GNU General Public License, version 3</a>. Integrated third-party "
        "components and archived algorithms retain their respective licenses "
        "and provenance.</p>");
    tabs->addTab(htmlPage(buildHtml, tabs), tr("Build and license"));

    QString librariesHtml = tr(
        "<h2>Geometry processing libraries</h2>"
        "<p>General-purpose libraries that many filters draw on.</p>");
    librariesHtml += thirdPartyList(kGeometryLibraries, std::size(kGeometryLibraries));
    librariesHtml += tr(
        "<h2>Vendored algorithm packages</h2>"
        "<p>Focused implementations kept alongside MeshLab, each behind the filters that "
        "use it, with its upstream history and license recorded next to the code.</p>");
    librariesHtml += thirdPartyList(kFocusedPackages, std::size(kFocusedPackages));
    librariesHtml += tr(
        "<p style='color:gray'>Each keeps its own license. TrueForm is not "
        "GPL-compatible and is included under a permission granted to this project "
        "specifically; anyone redistributing a build that contains it needs their own "
        "agreement. File formats, UI and numerics depend on further libraries that are "
        "not listed here.</p>");
    tabs->addTab(htmlPage(librariesHtml, tabs), tr("Libraries"));
    root->addWidget(tabs, 1);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok, this);
    auto *copyButton = buttons->addButton(tr("Copy Build Information"),
                                           QDialogButtonBox::ActionRole);
    connect(copyButton, &QPushButton::clicked, this, [] {
        QGuiApplication::clipboard()->setText(buildInformation());
    });
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    root->addWidget(buttons);
}
