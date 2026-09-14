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
