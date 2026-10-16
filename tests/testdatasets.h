#pragma once

// Locating test data that is too large to commit.
//
// Three directories hold it, all gitignored, and a test should not care which:
//
//   tests/datasets/<name>          fetched by tests/datasets/fetch.py
//   tests/local/<name>             a maintainer's own copy, never downloadable
//   .reference/meshlab/sample/<name>  a clone of classic MeshLab, whose sample/
//                                  tree carries the range maps the alignment
//                                  filters are tested against
//
// Nothing is fetched automatically. A test whose data is absent must SKIP, not
// fail: a fresh clone has none of it and still has to produce a green ctest.
// Before this header existed, two project-loading tests hard-coded paths under
// tests/ that only ever existed under tests/local, so they failed permanently
// on every machine but one.

#include <QFileInfo>
#include <QString>
#include <QStringList>
#include <QtTest>

namespace testdatasets {

// Where `name` is, or an empty string when it has not been fetched. `name` may
// include a path inside the dataset: datasetPath("redwood_rgbd_sample/depth").
inline QString datasetPath(const QString &name)
{
    const QStringList roots = {
        QStringLiteral(TEST_SOURCE_DIR "/tests/datasets/"),
        QStringLiteral(TEST_SOURCE_DIR "/tests/local/"),
        QStringLiteral(TEST_SOURCE_DIR "/.reference/meshlab/sample/")
    };
    for (const QString &root : roots) {
        const QString candidate = root + name;
        if (QFileInfo::exists(candidate))
            return candidate;
    }
    return {};
}

inline QString missingMessage(const QString &name)
{
    return QStringLiteral(
        "test data '%1' is not present. Fetch it with:\n"
        "    python3 tests/datasets/fetch.py\n"
        "or place it under tests/local/, or clone classic MeshLab into\n"
        "    .reference/meshlab  (its sample/ tree carries the range maps).").arg(name);
}

} // namespace testdatasets

// Binds `var` to the dataset path, or skips the test with an actionable message.
// A macro rather than a function because QSKIP returns from the caller.
#define MESHLAB2_REQUIRE_DATASET(var, name)                                        \
    const QString var = testdatasets::datasetPath(QStringLiteral(name));           \
    if (var.isEmpty())                                                             \
    QSKIP(qPrintable(testdatasets::missingMessage(QStringLiteral(name))))
