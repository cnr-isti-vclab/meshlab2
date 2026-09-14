#include "meshpluginregistry.h"

#include "meshiopluginmanager.h"

#if MESHLAB2_PLUGIN_IO_RAPIDOBJ_ENABLED
#include "plugins/io_obj_rapidobj/rapidobjimportplugin.h"
#endif

#if MESHLAB2_PLUGIN_IO_VCG_ENABLED
#include "plugins/io_vcg/vcgimportplugin.h"
#endif

#if MESHLAB2_PLUGIN_IO_E57_ENABLED
#include "plugins/io_e57/e57importplugin.h"
#endif

#if MESHLAB2_PLUGIN_IO_GLTF_ENABLED
#include "plugins/io_gltf/gltfimportplugin.h"
#endif

#if MESHLAB2_PLUGIN_IO_3MF_ENABLED
#include "plugins/io_3mf/threemfplugin.h"
#endif

#if MESHLAB2_PLUGIN_IO_TRUEFORM_ENABLED
#include "plugins/io_trueform/trueformioplugin.h"
#endif

// Registration order is the factory default. MeshIOPluginManager::pluginFor() honours the
// user's per-extension preference first and otherwise takes the FIRST registered plugin
// that accepts the file, so for an extension more than one plugin claims, whoever is listed
// first here wins for everybody who never opened the preference.
//
// Only two extensions are contested: .obj (vcglib, rapidobj, TrueForm) and .stl (vcglib,
// TrueForm). vcglib goes first for both because it is the most tolerant of the three -- the
// others are specialised, rapidobj for speed and TrueForm for geometry-only bulk loading --
// and a default has to cope with whatever a user drags onto it rather than be fast on the
// files it likes. Anyone who wants the faster importer can still pick it per extension.
void registerBuiltinMeshPlugins(MeshIOPluginManager &pluginManager)
{
#if MESHLAB2_PLUGIN_IO_VCG_ENABLED
    registerVcgImportPlugin(pluginManager);
#endif
#if MESHLAB2_PLUGIN_IO_RAPIDOBJ_ENABLED
    registerRapidObjImportPlugin(pluginManager);
#endif
#if MESHLAB2_PLUGIN_IO_E57_ENABLED
    registerE57ImportPlugin(pluginManager);
#endif
#if MESHLAB2_PLUGIN_IO_GLTF_ENABLED
    registerGltfImportPlugin(pluginManager);
#endif
#if MESHLAB2_PLUGIN_IO_3MF_ENABLED
    register3MFPlugin(pluginManager);
#endif
#if MESHLAB2_PLUGIN_IO_TRUEFORM_ENABLED
    registerTrueFormIOPlugin(pluginManager);
#endif
}
