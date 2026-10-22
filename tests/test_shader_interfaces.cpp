// On OpenGL a pipeline's vertex and fragment shaders are linked into one program, and a uniform
// both of them declare must have the same type in both. The GLSL that Qt bakes for its OpenGL
// backend has no uniform blocks: the block a stage declares as `ub` becomes a plain struct
// uniform, so a fragment shader that declares fewer members than its vertex shader declares a
// different struct under the same name, and the driver -- Mesa, on Linux -- refuses to link it.
// The pipeline is then not made, and nothing it would draw appears. Metal, Vulkan and D3D compile
// each stage on its own and never notice, which is how fragment blocks left behind when the
// clipping plane was added to their vertex shaders took points, wireframe, bounding box and the
// decorators off the screen on Linux only.
//
// GLSL 150 also matches what the vertex shader passes on to the fragment shader by name, not by
// location as the other backends do: a fragment input no vertex output of that name and type
// writes fails the link too.
//
// This reads the GLSL 150 the build baked for every vertex and fragment shader the renderer
// builds a pipeline from, and compares the uniforms the two stages share and the fragment
// shader's inputs with the vertex shader's outputs.
#include <QtTest/QtTest>

#include <rhi/qshader.h>

#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QSet>

class ShaderInterfaceTests : public QObject
{
    Q_OBJECT

private slots:
    void linkedStagesDeclareSharedUniformsAlike();
    void fragmentInputsAreVertexOutputsOfTheSameName();
};

namespace {

// The pairs the renderer loads: each vertex shader with the fragment shader loaded after it, in
// the order the sources load them (src/render/*.cpp).
QSet<QPair<QString, QString>> pipelineStagePairs()
{
    QSet<QPair<QString, QString>> pairs;
    const QDir renderDir(QStringLiteral(TEST_SOURCE_DIR "/src/render"));
    const QRegularExpression load(QStringLiteral(R"(:/shaders/(\w+)\.(vert|frag)\.qsb)"));
    for (const QString &name : renderDir.entryList({ QStringLiteral("*.cpp") }, QDir::Files)) {
        QFile file(renderDir.filePath(name));
        if (!file.open(QIODevice::ReadOnly))
            continue;
        const QString source = QString::fromUtf8(file.readAll());
        QString pendingVertex;
        for (auto it = load.globalMatch(source); it.hasNext();) {
            const QRegularExpressionMatch match = it.next();
            if (match.captured(2) == QLatin1String("vert"))
                pendingVertex = match.captured(1);
            else if (!pendingVertex.isEmpty())
                pairs.insert({ pendingVertex, match.captured(1) });
        }
    }
    return pairs;
}

QString openGlSource(const QString &shaderFile)
{
    QFile file(QStringLiteral(MESHLAB2_QSB_DIR "/") + shaderFile + QStringLiteral(".qsb"));
    if (!file.open(QIODevice::ReadOnly))
        return {};
    const QShader shader = QShader::fromSerialized(file.readAll());
    return QString::fromUtf8(
        shader.shader(QShaderKey(QShader::GlslShader, QShaderVersion(150))).shader());
}

// Each uniform the source declares, by name, with its type spelled out: a struct's members, in
// order, with the whitespace collapsed, so two declarations compare equal only when they match.
QHash<QString, QString> uniforms(const QString &source)
{
    QHash<QString, QString> structs;
    const QRegularExpression structDecl(QStringLiteral(R"(struct (\w+)\s*\{(.*?)\};)"),
                                        QRegularExpression::DotMatchesEverythingOption);
    for (auto it = structDecl.globalMatch(source); it.hasNext();) {
        const QRegularExpressionMatch match = it.next();
        structs.insert(match.captured(1), match.captured(2).simplified());
    }
    QHash<QString, QString> result;
    const QRegularExpression uniformDecl(QStringLiteral(R"(^uniform (\w+) (\w+);)"),
                                         QRegularExpression::MultilineOption);
    for (auto it = uniformDecl.globalMatch(source); it.hasNext();) {
        const QRegularExpressionMatch match = it.next();
        const QString type = match.captured(1);
        result.insert(match.captured(2), type + QStringLiteral(" { ") + structs.value(type) + QStringLiteral(" }"));
    }
    return result;
}

// The variables a stage passes in or out, by name, with their interpolation and type: "flat
// float" or "vec4". `direction` is "in" or "out"; the built-ins (gl_*) are left out.
QHash<QString, QString> interfaceVariables(const QString &source, const QString &direction)
{
    QHash<QString, QString> result;
    const QRegularExpression declaration(
        QStringLiteral(R"(^((?:flat |noperspective |smooth )?)%1 (\w+) (\w+)(\[\d+\])?;)").arg(direction),
        QRegularExpression::MultilineOption);
    for (auto it = declaration.globalMatch(source); it.hasNext();) {
        const QRegularExpressionMatch match = it.next();
        if (!match.captured(3).startsWith(QLatin1String("gl_")))
            result.insert(match.captured(3), match.captured(1) + match.captured(2) + match.captured(4));
    }
    return result;
}

} // namespace

void ShaderInterfaceTests::fragmentInputsAreVertexOutputsOfTheSameName()
{
    const QSet<QPair<QString, QString>> pairs = pipelineStagePairs();
    QVERIFY(pairs.size() >= 20);
    QStringList mismatches;
    for (const auto &[vertex, fragment] : pairs) {
        const QHash<QString, QString> outputs =
            interfaceVariables(openGlSource(vertex + QStringLiteral(".vert")), QStringLiteral("out"));
        const QHash<QString, QString> inputs =
            interfaceVariables(openGlSource(fragment + QStringLiteral(".frag")), QStringLiteral("in"));
        for (auto it = inputs.cbegin(); it != inputs.cend(); ++it) {
            if (!outputs.contains(it.key()))
                mismatches << QStringLiteral("%1.vert + %2.frag: '%3' is read but not written")
                                  .arg(vertex, fragment, it.key());
            else if (outputs.value(it.key()) != it.value())
                mismatches << QStringLiteral("%1.vert + %2.frag: '%3' is %4 out and %5 in")
                                  .arg(vertex, fragment, it.key(), outputs.value(it.key()), it.value());
        }
    }
    mismatches.sort();
    QVERIFY2(mismatches.isEmpty(), qPrintable(mismatches.join(QLatin1Char('\n'))));
}

void ShaderInterfaceTests::linkedStagesDeclareSharedUniformsAlike()
{
    const QSet<QPair<QString, QString>> pairs = pipelineStagePairs();
    // The renderer makes some two dozen pipelines; far fewer means the sources moved and the
    // pairs are no longer being found, not that every pair is fine.
    QVERIFY2(pairs.size() >= 20, qPrintable(QStringLiteral("only %1 pairs found").arg(pairs.size())));

    QStringList mismatches;
    for (const auto &[vertex, fragment] : pairs) {
        const QString vertexSource = openGlSource(vertex + QStringLiteral(".vert"));
        const QString fragmentSource = openGlSource(fragment + QStringLiteral(".frag"));
        QVERIFY2(!vertexSource.isEmpty(), qPrintable(vertex + QStringLiteral(".vert has no GLSL 150")));
        QVERIFY2(!fragmentSource.isEmpty(), qPrintable(fragment + QStringLiteral(".frag has no GLSL 150")));
        const QHash<QString, QString> vertexUniforms = uniforms(vertexSource);
        const QHash<QString, QString> fragmentUniforms = uniforms(fragmentSource);
        for (auto it = vertexUniforms.cbegin(); it != vertexUniforms.cend(); ++it) {
            if (fragmentUniforms.contains(it.key()) && fragmentUniforms.value(it.key()) != it.value())
                mismatches << QStringLiteral("%1.vert + %2.frag: '%3' is declared differently")
                                  .arg(vertex, fragment, it.key());
        }
    }
    mismatches.sort();
    QVERIFY2(mismatches.isEmpty(), qPrintable(mismatches.join(QLatin1Char('\n'))));
}

QTEST_MAIN(ShaderInterfaceTests)
#include "test_shader_interfaces.moc"
