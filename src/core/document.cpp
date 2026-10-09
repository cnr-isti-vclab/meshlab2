#include "document_internal.h"
#include "documentundomanager.h"

using namespace DocumentInternal;

std::uint64_t Document::nextInstanceId()
{
    static std::atomic<std::uint64_t> next{1};
    return next.fetch_add(1, std::memory_order_relaxed);
}

Document::Document(QObject *parent)
    : QObject(parent)
    , m_pluginManager(std::make_unique<MeshIOPluginManager>())
    , m_filterPluginManager(std::make_unique<MeshFilterPluginManager>())
    , m_gpuCache(std::make_unique<MeshGpuResourceCache>())
    , m_undoManager(std::make_unique<DocumentUndoManager>(*this))
{
    registerBuiltinMeshPlugins(*m_pluginManager);
    registerBuiltinMeshFilterPlugins(*m_filterPluginManager);
}

Document::~Document() = default;
