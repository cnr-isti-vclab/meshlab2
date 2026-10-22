#pragma once

#include <QDialog>
#include <QStringList>

class AboutDialog final : public QDialog
{
public:
    // `systemReport` is SystemInfo::graphicsReport() for the current view, and `warnings` the
    // recent Qt warnings: the System tab shows them after the build information, and copies all
    // of it for a bug report.
    AboutDialog(const QString &systemReport, const QStringList &warnings, QWidget *parent = nullptr);
};
