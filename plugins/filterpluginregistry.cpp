#include "filterpluginregistry.h"

#include "meshfilterpluginmanager.h"

#if MESHLAB2_PLUGIN_FILTER_BASIC_ENABLED
#include "plugins/filter_basic/basicfilterplugin.h"
#endif
#if MESHLAB2_PLUGIN_FILTER_EXPRESSION_ENABLED
#include "plugins/filter_expression/expressionfilterplugin.h"
#endif
#if MESHLAB2_PLUGIN_FILTER_EMBREE_ENABLED
#include "plugins/filter_embree/embreefilterplugin.h"
#endif
#if MESHLAB2_PLUGIN_FILTER_TRUEFORM_ENABLED
#include "plugins/filter_trueform/trueformfilterplugin.h"
#endif
#if MESHLAB2_PLUGIN_FILTER_SELECT_ENABLED
#include "plugins/filter_select/selectfilterplugin.h"
#endif
#if MESHLAB2_PLUGIN_FILTER_CLEAN_ENABLED
#include "plugins/filter_clean/cleanfilterplugin.h"
#endif
#if MESHLAB2_PLUGIN_FILTER_MESHFIX_ENABLED
#include "plugins/filter_meshfix/meshfixfilterplugin.h"
#endif
#if MESHLAB2_PLUGIN_FILTER_QSLIM_ENABLED
#include "plugins/filter_qslim/qslimfilterplugin.h"
#endif
#if MESHLAB2_PLUGIN_FILTER_INSTANT_MESHES_ENABLED
#include "plugins/filter_instant_meshes/instantmeshesfilterplugin.h"
#endif
#if MESHLAB2_PLUGIN_FILTER_QUADWILD_ENABLED
#include "plugins/filter_quadwild/quadwildfilterplugin.h"
#endif
#if MESHLAB2_PLUGIN_FILTER_MESHING_ENABLED
#include "plugins/filter_meshing/meshingfilterplugin.h"
#endif
#if MESHLAB2_PLUGIN_FILTER_CGAL_ENABLED
#include "plugins/filter_cgal/cgalfilterplugin.h"
#endif
#if MESHLAB2_PLUGIN_FILTER_GEOGRAM_ENABLED
#include "plugins/filter_geogram/geogramfilterplugin.h"
#endif
#if MESHLAB2_PLUGIN_FILTER_IGL_ENABLED
#include "plugins/filter_igl/iglfilterplugin.h"
#endif
#if MESHLAB2_PLUGIN_FILTER_SCREENED_POISSON_ENABLED
#include "plugins/filter_screened_poisson/screenedpoissonfilterplugin.h"
#endif
#if MESHLAB2_PLUGIN_FILTER_SAMPLING_ENABLED
#include "plugins/filter_sampling/samplingfilterplugin.h"
#endif
#if MESHLAB2_PLUGIN_FILTER_VORONOI_ENABLED
#include "plugins/filter_voronoi/voronoifilterplugin.h"
#endif
#if MESHLAB2_PLUGIN_FILTER_ICP_ENABLED
#include "plugins/filter_icp/icpfilterplugin.h"
#endif
#if MESHLAB2_PLUGIN_FILTER_UNSHARP_ENABLED
#include "plugins/filter_unsharp/unsharpfilterplugin.h"
#endif
#if MESHLAB2_PLUGIN_FILTER_CREATE_ENABLED
#include "plugins/filter_create/createfilterplugin.h"
#endif
#if MESHLAB2_PLUGIN_FILTER_GEODESIC_ENABLED
#include "plugins/filter_geodesic/geodesicfilterplugin.h"
#endif
#if MESHLAB2_PLUGIN_FILTER_TEXTURE_ENABLED
#include "plugins/filter_texture/texturefilterplugin.h"
#endif
#if MESHLAB2_PLUGIN_FILTER_TEXTURE_DEFRAGMENTATION_ENABLED
#include "plugins/filter_texture_defragmentation/texturedefragfilterplugin.h"
#endif
#if MESHLAB2_PLUGIN_FILTER_MEASURE_ENABLED
#include "plugins/filter_measure/measurefilterplugin.h"
#endif
#if MESHLAB2_PLUGIN_FILTER_MLS_ENABLED
#include "plugins/filter_mls/mlsfilterplugin.h"
#endif
#if MESHLAB2_PLUGIN_FILTER_VERTEX_DISPLACEMENT_ENABLED
#include "plugins/filter_vertex_displacement/vertexdisplacementfilterplugin.h"
#endif
#if MESHLAB2_PLUGIN_FILTER_LAYER_ENABLED
#include "plugins/filter_layer/layerfilterplugin.h"
#endif
#if MESHLAB2_PLUGIN_FILTER_COLORPROC_ENABLED
#include "plugins/filter_colorproc/colorprocfilterplugin.h"
#endif
#if MESHLAB2_PLUGIN_FILTER_ISOPARAM_ENABLED
#include "plugins/filter_isoparam/isoparamfilterplugin.h"
#endif
#if MESHLAB2_PLUGIN_FILTER_BPA_ENABLED
#include "plugins/filter_bpa/bpafilterplugin.h"
#endif
#if MESHLAB2_PLUGIN_FILTER_XATLAS_ENABLED
#include "plugins/filter_xatlas/xatlasfilterplugin.h"
#endif
#if MESHLAB2_PLUGIN_FILTER_TRIOPTIMIZE_ENABLED
#include "plugins/filter_trioptimize/trioptimizefilterplugin.h"
#endif
#if MESHLAB2_PLUGIN_FILTER_COLOR_PROJECTION_ENABLED
#include "plugins/filter_color_projection/colorprojectionfilterplugin.h"
#endif
#if MESHLAB2_PLUGIN_FILTER_CAMERA_ENABLED
#include "plugins/filter_camera/camerafilterplugin.h"
#endif
#if MESHLAB2_PLUGIN_FILTER_IMG_PATCH_PARAM_ENABLED
#include "plugins/filter_img_patch_param/imgpatchparamfilterplugin.h"
#endif
#if MESHLAB2_PLUGIN_FILTER_PLYMC_ENABLED
#include "plugins/filter_plymc/plymcfilterplugin.h"
#endif

void registerBuiltinMeshFilterPlugins(MeshFilterPluginManager &pluginManager)
{
#if MESHLAB2_PLUGIN_FILTER_BASIC_ENABLED
    registerBasicFilterPlugin(pluginManager);
#endif
#if MESHLAB2_PLUGIN_FILTER_EXPRESSION_ENABLED
    registerExpressionFilterPlugin(pluginManager);
#endif
#if MESHLAB2_PLUGIN_FILTER_EMBREE_ENABLED
    registerEmbreeFilterPlugin(pluginManager);
#endif
#if MESHLAB2_PLUGIN_FILTER_TRUEFORM_ENABLED
    registerTrueFormFilterPlugin(pluginManager);
#endif
#if MESHLAB2_PLUGIN_FILTER_SELECT_ENABLED
    registerSelectFilterPlugin(pluginManager);
#endif
#if MESHLAB2_PLUGIN_FILTER_CLEAN_ENABLED
    registerCleanFilterPlugin(pluginManager);
#endif
#if MESHLAB2_PLUGIN_FILTER_MESHFIX_ENABLED
    registerMeshFixFilterPlugin(pluginManager);
#endif
#if MESHLAB2_PLUGIN_FILTER_QSLIM_ENABLED
    registerQSlimFilterPlugin(pluginManager);
#endif
#if MESHLAB2_PLUGIN_FILTER_INSTANT_MESHES_ENABLED
    registerInstantMeshesFilterPlugin(pluginManager);
#endif
#if MESHLAB2_PLUGIN_FILTER_QUADWILD_ENABLED
    registerQuadWildFilterPlugin(pluginManager);
#endif
#if MESHLAB2_PLUGIN_FILTER_MESHING_ENABLED
    registerMeshingFilterPlugin(pluginManager);
#endif
#if MESHLAB2_PLUGIN_FILTER_CGAL_ENABLED
    registerCgalFilterPlugin(pluginManager);
#endif
#if MESHLAB2_PLUGIN_FILTER_GEOGRAM_ENABLED
    registerGeogramFilterPlugin(pluginManager);
#endif
#if MESHLAB2_PLUGIN_FILTER_IGL_ENABLED
    registerIglFilterPlugin(pluginManager);
#endif
#if MESHLAB2_PLUGIN_FILTER_SCREENED_POISSON_ENABLED
    registerScreenedPoissonFilterPlugin(pluginManager);
#endif
#if MESHLAB2_PLUGIN_FILTER_SAMPLING_ENABLED
    registerSamplingFilterPlugin(pluginManager);
#endif
#if MESHLAB2_PLUGIN_FILTER_VORONOI_ENABLED
    registerVoronoiFilterPlugin(pluginManager);
#endif
#if MESHLAB2_PLUGIN_FILTER_ICP_ENABLED
    registerIcpFilterPlugin(pluginManager);
#endif
#if MESHLAB2_PLUGIN_FILTER_UNSHARP_ENABLED
    registerUnsharpFilterPlugin(pluginManager);
#endif
#if MESHLAB2_PLUGIN_FILTER_CREATE_ENABLED
    registerCreateFilterPlugin(pluginManager);
#endif
#if MESHLAB2_PLUGIN_FILTER_GEODESIC_ENABLED
    registerGeodesicFilterPlugin(pluginManager);
#endif
#if MESHLAB2_PLUGIN_FILTER_TEXTURE_ENABLED
    registerTextureFilterPlugin(pluginManager);
#endif
#if MESHLAB2_PLUGIN_FILTER_TEXTURE_DEFRAGMENTATION_ENABLED
    registerTextureDefragFilterPlugin(pluginManager);
#endif
#if MESHLAB2_PLUGIN_FILTER_MEASURE_ENABLED
    registerMeasureFilterPlugin(pluginManager);
#endif
#if MESHLAB2_PLUGIN_FILTER_MLS_ENABLED
    registerMlsFilterPlugin(pluginManager);
#endif
#if MESHLAB2_PLUGIN_FILTER_VERTEX_DISPLACEMENT_ENABLED
    registerVertexDisplacementFilterPlugin(pluginManager);
#endif
#if MESHLAB2_PLUGIN_FILTER_LAYER_ENABLED
    registerLayerFilterPlugin(pluginManager);
#endif
#if MESHLAB2_PLUGIN_FILTER_COLORPROC_ENABLED
    registerColorProcFilterPlugin(pluginManager);
#endif
#if MESHLAB2_PLUGIN_FILTER_ISOPARAM_ENABLED
    registerIsoParamFilterPlugin(pluginManager);
#endif
#if MESHLAB2_PLUGIN_FILTER_BPA_ENABLED
    registerBpaFilterPlugin(pluginManager);
#endif
#if MESHLAB2_PLUGIN_FILTER_XATLAS_ENABLED
    registerXAtlasFilterPlugin(pluginManager);
#endif
#if MESHLAB2_PLUGIN_FILTER_TRIOPTIMIZE_ENABLED
    registerTriOptimizeFilterPlugin(pluginManager);
#endif
#if MESHLAB2_PLUGIN_FILTER_COLOR_PROJECTION_ENABLED
    registerColorProjectionFilterPlugin(pluginManager);
#endif
#if MESHLAB2_PLUGIN_FILTER_CAMERA_ENABLED
    registerCameraFilterPlugin(pluginManager);
#endif
#if MESHLAB2_PLUGIN_FILTER_IMG_PATCH_PARAM_ENABLED
    registerImgPatchParamFilterPlugin(pluginManager);
#endif
#if MESHLAB2_PLUGIN_FILTER_PLYMC_ENABLED
    registerPlyMCFilterPlugin(pluginManager);
#endif
}
