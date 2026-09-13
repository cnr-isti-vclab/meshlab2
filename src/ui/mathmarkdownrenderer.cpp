#include "mathmarkdownrenderer.h"

#include <QHash>
#include <QImage>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextImageFormat>
#include <QVector>

#ifdef MESHLAB2_MATH_HELP
#include <jkqtmathtext/jkqtmathtext.h>
#endif

namespace {

bool isEscaped(const QString &text, int position)
{
    int slashes = 0;
    while (position > 0 && text.at(--position) == QLatin1Char('\\'))
        ++slashes;
    return (slashes % 2) != 0;
}

int closingDelimiter(const QString &text, int from, int delimiterLength)
{
    for (int i = from; i < text.size(); ++i) {
        if (delimiterLength == 1 && text.at(i) == QLatin1Char('\n'))
            return -1;
        if (text.at(i) != QLatin1Char('$') || isEscaped(text, i))
            continue;
        if (delimiterLength == 2) {
            if (i + 1 < text.size() && text.at(i + 1) == QLatin1Char('$'))
                return i;
        } else if ((i == 0 || text.at(i - 1) != QLatin1Char('$'))
                   && (i + 1 == text.size() || text.at(i + 1) != QLatin1Char('$'))) {
            return i;
        }
    }
    return -1;
}

#ifdef MESHLAB2_MATH_HELP
QImage formulaImage(
    const QString &latex,
    bool display,
    const QFont &font,
    const QColor &color,
    qreal devicePixelRatio)
{
    const qreal pointSize = font.pointSizeF() > 0 ? font.pointSizeF() : 12.0;
    const QString key = QStringLiteral("%1|%2|%3|%4|%5")
                            .arg(latex)
                            .arg(display)
                            .arg(pointSize)
                            .arg(color.rgba())
                            .arg(devicePixelRatio);
    static QHash<QString, QImage> cache;
    const auto cached = cache.constFind(key);
    if (cached != cache.cend())
        return *cached;

    JKQTMathText math;
    math.useFiraMath();
    math.setFontColor(color);
    math.setFontPointSize(pointSize * (display ? 1.25 : 1.15));
    const auto options =
        JKQTMathText::ParseOptions(JKQTMathText::StartWithMathMode)
        | JKQTMathText::AllowLinebreaks;
    if (!math.parse(latex, JKQTMathText::LatexParser, options) || math.hadErrors())
        return {};

    QImage image = math.drawIntoImage(
        false, QColor(Qt::transparent), 1, devicePixelRatio);
    if (cache.size() >= 128)
        cache.clear();
    cache.insert(key, image);
    return image;
}
#endif

struct FormulaResource
{
    QString placeholder;
    QImage image;
    bool display = false;
};

} // namespace

void MathMarkdownRenderer::setMarkdown(QTextBrowser &browser, const QString &markdown)
{
#ifndef MESHLAB2_MATH_HELP
    browser.setMarkdown(markdown);
#else
    QString rendered;
    rendered.reserve(markdown.size());
    QVector<FormulaResource> formulas;
    int copiedUntil = 0;
    int codeTicks = 0;

    for (int i = 0; i < markdown.size();) {
        if (markdown.at(i) == QLatin1Char('`')) {
            int run = 1;
            while (i + run < markdown.size()
                   && markdown.at(i + run) == QLatin1Char('`')) {
                ++run;
            }
            if (codeTicks == 0)
                codeTicks = run;
            else if (run == codeTicks)
                codeTicks = 0;
            i += run;
            continue;
        }
        if (codeTicks != 0 || markdown.at(i) != QLatin1Char('$')
            || isEscaped(markdown, i)) {
            ++i;
            continue;
        }

        const int delimiterLength =
            i + 1 < markdown.size() && markdown.at(i + 1) == QLatin1Char('$')
            ? 2
            : 1;
        const int closing =
            closingDelimiter(markdown, i + delimiterLength, delimiterLength);
        if (closing < 0) {
            ++i;
            continue;
        }

        const QString latex =
            markdown.mid(i + delimiterLength, closing - i - delimiterLength).trimmed();
        const bool display = delimiterLength == 2;
        const QImage image = latex.isEmpty()
            ? QImage()
            : formulaImage(
                  latex,
                  display,
                  browser.font(),
                  browser.palette().color(QPalette::Text),
                  browser.devicePixelRatioF());
        if (image.isNull()) {
            i = closing + delimiterLength;
            continue;
        }

        rendered += markdown.mid(copiedUntil, i - copiedUntil);
        const QString placeholder =
            QStringLiteral("QMESHLABFORMULA%1TOKEN").arg(formulas.size());
        rendered += display
            ? QStringLiteral("\n\n%1\n\n").arg(placeholder)
            : placeholder;
        formulas.push_back({placeholder, image, display});
        i = closing + delimiterLength;
        copiedUntil = i;
    }
    rendered += markdown.mid(copiedUntil);

    browser.setMarkdown(rendered);
    QTextDocument *document = browser.document();
    for (const FormulaResource &formula : formulas) {
        document->addResource(
            QTextDocument::ImageResource, QUrl(formula.placeholder), formula.image);
        QTextCursor cursor = document->find(formula.placeholder);
        if (cursor.isNull())
            continue;

        QTextImageFormat imageFormat;
        imageFormat.setName(formula.placeholder);
        imageFormat.setVerticalAlignment(QTextCharFormat::AlignMiddle);
        cursor.insertImage(imageFormat);
        if (formula.display) {
            QTextBlockFormat format = cursor.blockFormat();
            format.setAlignment(Qt::AlignCenter);
            cursor.setBlockFormat(format);
        }
    }
    document->markContentsDirty(0, document->characterCount());
#endif
}
