#include "meshset_core.h"
#include "pymesh.h"

#include "document.h"

#if !defined(PYMESHLAB_STANDALONE)
#include "headlessrendercontext.h"
#endif

#include <nanobind/stl/array.h>

#include <QColor>
#include <QCoreApplication>
#include <QImage>
#include <QVariant>
#include <QVector3D>

#include <memory>
#include <stdexcept>

namespace nb = nanobind;

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

namespace {

QString toQString(const std::string &s)
{
    return QString::fromUtf8(s.c_str());
}

std::string toStdString(const QString &s)
{
    const QByteArray bytes = s.toUtf8();
    return std::string(bytes.constData(), static_cast<size_t>(bytes.size()));
}

std::string pyUnicodeToStdString(PyObject *obj)
{
    if (!PyUnicode_Check(obj))
        throw std::runtime_error("expected a string");

    Py_ssize_t size = 0;
    const char *utf8 = PyUnicode_AsUTF8AndSize(obj, &size);
    if (!utf8) {
        PyErr_Clear();
        throw std::runtime_error("failed to read unicode string");
    }
    return std::string(utf8, static_cast<size_t>(size));
}

std::vector<std::string> toStdVector(const QStringList &list)
{
    std::vector<std::string> out;
    out.reserve(static_cast<size_t>(list.size()));
    for (const QString &entry : list)
        out.push_back(toStdString(entry));
    return out;
}

// Ensures a QCoreApplication exists; no-op when called from inside the Qt app.
void ensureQCoreApplication()
{
    if (QCoreApplication::instance())
        return;

    static int argc = 1;
    static char arg0[] = "qmeshlab_python";
    static char *argv[] = { arg0, nullptr };
    static QCoreApplication app(argc, argv);
    (void) app;
}

QVariant pythonValueToQVariant(const nb::handle &value)
{
    PyObject *obj = value.ptr();

    if (obj == Py_None)
        return QVariant();

    if (PyBool_Check(obj))
        return QVariant(obj == Py_True);

    if (PyLong_Check(obj) && !PyBool_Check(obj)) {
        const long long intValue = PyLong_AsLongLong(obj);
        if (PyErr_Occurred()) {
            PyErr_Clear();
            throw std::runtime_error("invalid integer value");
        }
        return QVariant::fromValue(static_cast<qlonglong>(intValue));
    }

    if (PyFloat_Check(obj))
        return QVariant(PyFloat_AsDouble(obj));

    if (PyUnicode_Check(obj))
        return QVariant(toQString(pyUnicodeToStdString(obj)));

    if (PyTuple_Check(obj) || PyList_Check(obj)) {
        const Py_ssize_t size = PySequence_Size(obj);
        if (size == 3) {
            float coords[3];
            for (Py_ssize_t i = 0; i < 3; ++i) {
                PyObject *item = PySequence_GetItem(obj, i);
                if (!item)
                    throw std::runtime_error("invalid point3f sequence");
                const double coordinate = PyFloat_AsDouble(item);
                Py_DECREF(item);
                if (PyErr_Occurred()) {
                    PyErr_Clear();
                    throw std::runtime_error("point3f coordinates must be numeric");
                }
                coords[i] = static_cast<float>(coordinate);
            }
            return QVariant::fromValue(QVector3D(coords[0], coords[1], coords[2]));
        }
    }

    throw std::runtime_error("Unsupported parameter type in filter params dictionary.");
}

MeshFilterParameterValues pythonDictToFilterParams(const nb::dict &params)
{
    MeshFilterParameterValues out;

    PyObject *dictObj = params.ptr();
    if (!PyDict_Check(dictObj))
        throw std::runtime_error("Filter params must be a dictionary.");

    PyObject *pyKey = nullptr;
    PyObject *pyValue = nullptr;
    Py_ssize_t pos = 0;
    while (PyDict_Next(dictObj, &pos, &pyKey, &pyValue)) {
        const nb::handle valueHandle(pyValue);
        const std::string key = pyUnicodeToStdString(pyKey);
        try {
            out.insert(toQString(key), pythonValueToQVariant(valueHandle));
        } catch (const std::exception &e) {
            throw std::runtime_error(
                std::string("Failed to convert parameter '") + key + "': " + e.what());
        }
    }
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// MeshSetCore
// ---------------------------------------------------------------------------

MeshSetCore::MeshSetCore()
    : m_document(nullptr)
    , m_ownsDocument(true)
{
    ensureQCoreApplication();
    m_document = new Document();
}

MeshSetCore::MeshSetCore(Document *doc)
    : m_document(doc)
    , m_ownsDocument(false)
{
    // The caller (embedded app) owns the Document; we just borrow it.
}

MeshSetCore::~MeshSetCore()
{
    if (m_ownsDocument)
        delete m_document;
    m_document = nullptr;
}

int MeshSetCore::meshCount() const
{
    return m_document->meshCount();
}

int MeshSetCore::currentMeshIndex() const
{
    return m_document->currentMeshIndex();
}

int MeshSetCore::currentMeshId() const
{
    const int index = m_document->currentMeshIndex();
    if (index < 0 || index >= m_document->meshCount())
        throw std::runtime_error("MeshSet has no current Mesh.");
    return index;
}

void MeshSetCore::setCurrentMesh(int index)
{
    if (index < 0 || index >= m_document->meshCount())
        throw std::runtime_error("Mesh index out of range.");
    m_document->setCurrentMeshIndex(index);
}

bool MeshSetCore::meshIdExists(int index) const
{
    return index >= 0 && index < m_document->meshCount();
}

nanobind::object MeshSetCore::currentMesh() const
{
    const int idx = currentMeshId();
    return nb::cast(PyMesh(m_document, idx));
}

nanobind::object MeshSetCore::mesh(int index) const
{
    if (index < 0 || index >= m_document->meshCount())
        throw std::runtime_error("Mesh index out of range.");
    return nb::cast(PyMesh(m_document, index));
}

void MeshSetCore::setCurrentMeshVisibility(bool visible)
{
    const int index = currentMeshId();
    m_document->setMeshVisible(index, visible);
}

void MeshSetCore::setMeshVisibility(int index, bool visible)
{
    if (!meshIdExists(index))
        throw std::runtime_error("Mesh index out of range.");
    m_document->setMeshVisible(index, visible);
}

bool MeshSetCore::isCurrentMeshVisible() const
{
    return isMeshVisible(currentMeshId());
}

bool MeshSetCore::isMeshVisible(int index) const
{
    if (!meshIdExists(index))
        throw std::runtime_error("Mesh index out of range.");
    return m_document->mesh(index).visible;
}

void MeshSetCore::loadNewMesh(const std::string &path)
{
    const int err = m_document->loadMesh(toQString(path));
    if (err != 0)
        throw std::runtime_error("Failed to load mesh: " + path);
}

void MeshSetCore::saveCurrentMesh(const std::string &path)
{
    const int err = m_document->saveCurrentMesh(toQString(path));
    if (err != 0)
        throw std::runtime_error("Failed to save current mesh: " + path);
}

int MeshSetCore::rasterCount() const
{
    return m_document->rasterCount();
}

int MeshSetCore::currentRasterIndex() const
{
    return m_document->currentRasterIndex();
}

void MeshSetCore::setCurrentRaster(int index)
{
    if (index < 0 || index >= m_document->rasterCount())
        throw std::runtime_error("Raster index out of range.");
    m_document->setCurrentRasterIndex(index);
}

void MeshSetCore::loadRasterImage(const std::string &path)
{
    const int idx = m_document->loadRasterImage(toQString(path));
    if (idx < 0)
        throw std::runtime_error("Failed to load raster image: " + path);
}

void MeshSetCore::clear()
{
    while (m_document->rasterCount() > 0)
        m_document->removeRaster(m_document->rasterCount() - 1);
    while (m_document->meshCount() > 0)
        m_document->removeMesh(m_document->meshCount() - 1);
    m_document->clearUndoHistory();
    m_document->clearLog();
}

void MeshSetCore::loadProject(const std::string &path)
{
    const int err = m_document->loadMeshLabProject(toQString(path));
    if (err != 0)
        throw std::runtime_error("Failed to load project: " + path);
}

void MeshSetCore::saveProject(const std::string &path)
{
    QString error;
    Document::MeshLabProjectSaveOptions options;
    if (!m_document->saveMeshLabProject(toQString(path), options, &error)) {
        const std::string message = error.isEmpty()
            ? std::string("Failed to save project: ") + path
            : toStdString(error);
        throw std::runtime_error(message);
    }
}

std::vector<std::string> MeshSetCore::filterList() const
{
    std::vector<std::string> names;
    const auto filters = listFilters();
    names.reserve(filters.size());
    for (const auto &filter : filters) {
        if (!filter.python_name.empty())
            names.push_back(filter.python_name);
    }
    return names;
}

std::vector<FilterInfoRecord> MeshSetCore::listFilters() const
{
    const auto filters = m_document->filterInfos();
    std::vector<FilterInfoRecord> out;
    out.reserve(filters.size());
    for (const auto &info : filters) {
        FilterInfoRecord item;
        item.key = toStdString(info.key);
        item.id = toStdString(info.descriptor.id);
        item.plugin_id = toStdString(info.pluginId);
        item.plugin_name = toStdString(info.pluginName);
        item.name = toStdString(info.descriptor.name);
        item.python_name = toStdString(info.descriptor.effectivePythonName());
        item.applicable = info.applicable;
        item.applicability_error = toStdString(info.applicabilityError);
        out.push_back(std::move(item));
    }
    return out;
}

QString MeshSetCore::resolveFilterKey(const QString &filterNameOrKey) const
{
    if (filterNameOrKey.contains(QStringLiteral("::")))
        return filterNameOrKey;

    const auto infos = m_document->filterInfos();

    // First pass: match by filter id (e.g. "Remove Unreferenced Vertices")
    QString resolved;
    for (const auto &info : infos) {
        if (info.descriptor.id != filterNameOrKey)
            continue;
        if (!resolved.isEmpty())
            throw std::runtime_error(
                "Ambiguous filter id; use fully qualified key (pluginId::filterId).");
        resolved = info.key;
    }
    if (!resolved.isEmpty())
        return resolved;

    // Second pass: match by python_name (e.g. "remove_duplicate_vertices")
    const std::string candidate = toStdString(filterNameOrKey);
    for (const auto &info : infos) {
        if (toStdString(info.descriptor.effectivePythonName()) != candidate)
            continue;
        if (!resolved.isEmpty())
            throw std::runtime_error(
                "Ambiguous python filter name; use fully qualified key (pluginId::filterId).");
        resolved = info.key;
    }
    if (!resolved.isEmpty())
        return resolved;

    throw std::runtime_error("Unknown filter: " + toStdString(filterNameOrKey));
}

FilterRunRecord MeshSetCore::applyFilter(const std::string &filterNameOrKey,
                                         const nb::dict &params) const
{
    try {
        const QString key = resolveFilterKey(toQString(filterNameOrKey));

        MeshFilterParameterValues qParams;
        try {
            qParams = pythonDictToFilterParams(params);
        } catch (const std::exception &e) {
            throw std::runtime_error(std::string("parameter conversion failed: ") + e.what());
        }

        MeshFilterRunResult result;
        try {
            result = m_document->runFilter(key, qParams);
        } catch (const std::exception &e) {
            throw std::runtime_error(std::string("backend filter execution failed: ") + e.what());
        }

        FilterRunRecord out;
        out.success = result.success;
        out.document_modified = result.documentModified;
        out.error_message = toStdString(result.errorMessage);
        out.info_messages = toStdVector(result.infoMessages);
        out.new_mesh_indices.reserve(static_cast<size_t>(result.newMeshIndices.size()));
        for (int index : result.newMeshIndices)
            out.new_mesh_indices.push_back(index);
        out.output_values = result.outputValues;

        if (!out.success)
            throw std::runtime_error(out.error_message.empty() ? "Filter failed." : out.error_message);

        return out;
    } catch (const std::exception &e) {
        throw std::runtime_error(std::string("apply_filter failed: ") + e.what());
    }
}

#if !defined(PYMESHLAB_STANDALONE)
nanobind::bytes MeshSetCore::renderSnapshot(
    const std::string &renderStateJson,
    int width,
    int height)
{
    if (!m_document)
        throw std::runtime_error("No document loaded");

    if (width <= 0 || height <= 0)
        throw std::runtime_error("Invalid output size");

    if (!m_renderContext) {
        m_renderContext = std::make_unique<HeadlessRenderContext>(m_document);
        if (!m_renderContext->isValid())
            throw std::runtime_error("HeadlessRenderContext could not initialise (no GPU or offscreen platform?)");
    }

    QString json = QString::fromUtf8(renderStateJson.c_str(), int(renderStateJson.size()));
    QString errorStr;
    QImage image = m_renderContext->snapshot(json, QSize(width, height), false, &errorStr);

    if (image.isNull())
        throw std::runtime_error("Render failed: " + errorStr.toStdString());

    // Convert to RGBA and return raw bytes
    if (image.format() != QImage::Format_RGBA8888)
        image = image.convertToFormat(QImage::Format_RGBA8888);

    const size_t byteCount = size_t(image.sizeInBytes());
    return nanobind::bytes(
        reinterpret_cast<const char *>(image.constBits()),
        byteCount);
}
#endif
