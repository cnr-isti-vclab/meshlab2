add_library(MeshLab2Plugins STATIC
    meshpluginregistry.h
    meshpluginregistry.cpp
    filterpluginregistry.h
    filterpluginregistry.cpp
)

target_include_directories(MeshLab2Plugins
    PUBLIC
        ${MESHLAB2_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(MeshLab2Plugins
    PUBLIC
        MeshLab2Core
        Qt6::Core
)

add_subdirectory(io_obj_rapidobj)
if (TARGET MeshLab2PluginIORapidObj)
    target_link_libraries(MeshLab2Plugins PUBLIC MeshLab2PluginIORapidObj)
    target_compile_definitions(MeshLab2Plugins PRIVATE MESHLAB2_PLUGIN_IO_RAPIDOBJ_ENABLED=1)
endif()

add_subdirectory(io_vcg)
if (TARGET MeshLab2PluginIOVCG)
    target_link_libraries(MeshLab2Plugins PUBLIC MeshLab2PluginIOVCG)
    target_compile_definitions(MeshLab2Plugins PRIVATE MESHLAB2_PLUGIN_IO_VCG_ENABLED=1)
endif()

add_subdirectory(io_e57)
if (TARGET MeshLab2PluginIOE57)
    target_link_libraries(MeshLab2Plugins PUBLIC MeshLab2PluginIOE57)
    target_compile_definitions(MeshLab2Plugins PRIVATE MESHLAB2_PLUGIN_IO_E57_ENABLED=1)
endif()

add_subdirectory(io_gltf)
if (TARGET MeshLab2PluginIOGLTF)
    target_link_libraries(MeshLab2Plugins PUBLIC MeshLab2PluginIOGLTF)
    target_compile_definitions(MeshLab2Plugins PRIVATE MESHLAB2_PLUGIN_IO_GLTF_ENABLED=1)
endif()

add_subdirectory(io_3mf)
if (TARGET MeshLab2PluginIO3MF)
    target_link_libraries(MeshLab2Plugins PUBLIC MeshLab2PluginIO3MF)
    target_compile_definitions(MeshLab2Plugins PRIVATE MESHLAB2_PLUGIN_IO_3MF_ENABLED=1)
endif()

add_subdirectory(io_trueform)
if (TARGET MeshLab2PluginIOTrueForm)
    target_link_libraries(MeshLab2Plugins PUBLIC MeshLab2PluginIOTrueForm)
    target_compile_definitions(MeshLab2Plugins PRIVATE MESHLAB2_PLUGIN_IO_TRUEFORM_ENABLED=1)
endif()

add_subdirectory(filter_trueform)
if (TARGET MeshLab2PluginFilterTrueForm)
    target_link_libraries(MeshLab2Plugins PUBLIC MeshLab2PluginFilterTrueForm)
    target_compile_definitions(MeshLab2Plugins PRIVATE MESHLAB2_PLUGIN_FILTER_TRUEFORM_ENABLED=1)
endif()

add_subdirectory(filter_basic)
if (TARGET MeshLab2PluginFilterBasic)
    target_link_libraries(MeshLab2Plugins PUBLIC MeshLab2PluginFilterBasic)
    target_compile_definitions(MeshLab2Plugins PRIVATE MESHLAB2_PLUGIN_FILTER_BASIC_ENABLED=1)
endif()

add_subdirectory(filter_expression)
if (TARGET MeshLab2PluginFilterExpression)
    target_link_libraries(MeshLab2Plugins PUBLIC MeshLab2PluginFilterExpression)
    target_compile_definitions(MeshLab2Plugins PRIVATE MESHLAB2_PLUGIN_FILTER_EXPRESSION_ENABLED=1)
endif()

add_subdirectory(filter_embree)
if (TARGET MeshLab2PluginFilterEmbree)
    target_link_libraries(MeshLab2Plugins PUBLIC MeshLab2PluginFilterEmbree)
    target_compile_definitions(MeshLab2Plugins PRIVATE MESHLAB2_PLUGIN_FILTER_EMBREE_ENABLED=1)
endif()

add_subdirectory(filter_select)
if (TARGET MeshLab2PluginFilterSelect)
    target_link_libraries(MeshLab2Plugins PUBLIC MeshLab2PluginFilterSelect)
    target_compile_definitions(MeshLab2Plugins PRIVATE MESHLAB2_PLUGIN_FILTER_SELECT_ENABLED=1)
endif()

add_subdirectory(filter_clean)
if (TARGET MeshLab2PluginFilterClean)
    target_link_libraries(MeshLab2Plugins PUBLIC MeshLab2PluginFilterClean)
    target_compile_definitions(MeshLab2Plugins PRIVATE MESHLAB2_PLUGIN_FILTER_CLEAN_ENABLED=1)
endif()

add_subdirectory(filter_meshfix)
if (TARGET MeshLab2PluginFilterMeshFix)
    target_link_libraries(MeshLab2Plugins PUBLIC MeshLab2PluginFilterMeshFix)
    target_compile_definitions(MeshLab2Plugins PRIVATE MESHLAB2_PLUGIN_FILTER_MESHFIX_ENABLED=1)
endif()

add_subdirectory(filter_qslim)
if (TARGET MeshLab2PluginFilterQSlim)
    target_link_libraries(MeshLab2Plugins PUBLIC MeshLab2PluginFilterQSlim)
    target_compile_definitions(MeshLab2Plugins PRIVATE MESHLAB2_PLUGIN_FILTER_QSLIM_ENABLED=1)
endif()

add_subdirectory(filter_instant_meshes)
if (TARGET MeshLab2PluginFilterInstantMeshes)
    target_link_libraries(MeshLab2Plugins PUBLIC MeshLab2PluginFilterInstantMeshes)
    target_compile_definitions(MeshLab2Plugins PRIVATE MESHLAB2_PLUGIN_FILTER_INSTANT_MESHES_ENABLED=1)
endif()

add_subdirectory(filter_quadwild)
if (TARGET MeshLab2PluginFilterQuadWild)
    target_link_libraries(MeshLab2Plugins PUBLIC MeshLab2PluginFilterQuadWild)
    target_compile_definitions(MeshLab2Plugins PRIVATE MESHLAB2_PLUGIN_FILTER_QUADWILD_ENABLED=1)
endif()

add_subdirectory(filter_meshing)
if (TARGET MeshLab2PluginFilterMeshing)
    target_link_libraries(MeshLab2Plugins PUBLIC MeshLab2PluginFilterMeshing)
    target_compile_definitions(MeshLab2Plugins PRIVATE MESHLAB2_PLUGIN_FILTER_MESHING_ENABLED=1)
endif()

add_subdirectory(filter_cgal)
if (TARGET MeshLab2PluginFilterCGAL)
    target_link_libraries(MeshLab2Plugins PUBLIC MeshLab2PluginFilterCGAL)
    target_compile_definitions(MeshLab2Plugins PRIVATE MESHLAB2_PLUGIN_FILTER_CGAL_ENABLED=1)
endif()

add_subdirectory(filter_geogram)
if (TARGET MeshLab2PluginFilterGeogram)
    target_link_libraries(MeshLab2Plugins PUBLIC MeshLab2PluginFilterGeogram)
    target_compile_definitions(MeshLab2Plugins PRIVATE MESHLAB2_PLUGIN_FILTER_GEOGRAM_ENABLED=1)
endif()

add_subdirectory(filter_igl)
if (TARGET MeshLab2PluginFilterIgl)
    target_link_libraries(MeshLab2Plugins PUBLIC MeshLab2PluginFilterIgl)
    target_compile_definitions(MeshLab2Plugins PRIVATE MESHLAB2_PLUGIN_FILTER_IGL_ENABLED=1)
endif()

add_subdirectory(filter_screened_poisson)
if (TARGET MeshLab2PluginFilterScreenedPoisson)
    target_link_libraries(MeshLab2Plugins PUBLIC MeshLab2PluginFilterScreenedPoisson)
    target_compile_definitions(MeshLab2Plugins PRIVATE MESHLAB2_PLUGIN_FILTER_SCREENED_POISSON_ENABLED=1)
endif()

add_subdirectory(filter_sampling)
if (TARGET MeshLab2PluginFilterSampling)
    target_link_libraries(MeshLab2Plugins PUBLIC MeshLab2PluginFilterSampling)
    target_compile_definitions(MeshLab2Plugins PRIVATE MESHLAB2_PLUGIN_FILTER_SAMPLING_ENABLED=1)
endif()

add_subdirectory(filter_voronoi)
if (TARGET MeshLab2PluginFilterVoronoi)
    target_link_libraries(MeshLab2Plugins PUBLIC MeshLab2PluginFilterVoronoi)
    target_compile_definitions(MeshLab2Plugins PRIVATE MESHLAB2_PLUGIN_FILTER_VORONOI_ENABLED=1)
endif()

add_subdirectory(filter_icp)
if (TARGET MeshLab2PluginFilterICP)
    target_link_libraries(MeshLab2Plugins PUBLIC MeshLab2PluginFilterICP)
    target_compile_definitions(MeshLab2Plugins PRIVATE MESHLAB2_PLUGIN_FILTER_ICP_ENABLED=1)
endif()

add_subdirectory(filter_unsharp)
if (TARGET MeshLab2PluginFilterUnsharp)
    target_link_libraries(MeshLab2Plugins PUBLIC MeshLab2PluginFilterUnsharp)
    target_compile_definitions(MeshLab2Plugins PRIVATE MESHLAB2_PLUGIN_FILTER_UNSHARP_ENABLED=1)
endif()

add_subdirectory(filter_create)
if (TARGET MeshLab2PluginFilterCreate)
    target_link_libraries(MeshLab2Plugins PUBLIC MeshLab2PluginFilterCreate)
    target_compile_definitions(MeshLab2Plugins PRIVATE MESHLAB2_PLUGIN_FILTER_CREATE_ENABLED=1)
endif()

add_subdirectory(filter_geodesic)
if (TARGET MeshLab2PluginFilterGeodesic)
    target_link_libraries(MeshLab2Plugins PUBLIC MeshLab2PluginFilterGeodesic)
    target_compile_definitions(MeshLab2Plugins PRIVATE MESHLAB2_PLUGIN_FILTER_GEODESIC_ENABLED=1)
endif()

add_subdirectory(filter_texture)
if (TARGET MeshLab2PluginFilterTexture)
    target_link_libraries(MeshLab2Plugins PUBLIC MeshLab2PluginFilterTexture)
    target_compile_definitions(MeshLab2Plugins PRIVATE MESHLAB2_PLUGIN_FILTER_TEXTURE_ENABLED=1)
endif()

add_subdirectory(filter_texture_defragmentation)
if (TARGET MeshLab2PluginFilterTextureDefragmentation)
    target_link_libraries(MeshLab2Plugins PUBLIC MeshLab2PluginFilterTextureDefragmentation)
    target_compile_definitions(MeshLab2Plugins PRIVATE MESHLAB2_PLUGIN_FILTER_TEXTURE_DEFRAGMENTATION_ENABLED=1)
endif()

add_subdirectory(filter_measure)
if (TARGET MeshLab2PluginFilterMeasure)
    target_link_libraries(MeshLab2Plugins PUBLIC MeshLab2PluginFilterMeasure)
    target_compile_definitions(MeshLab2Plugins PRIVATE MESHLAB2_PLUGIN_FILTER_MEASURE_ENABLED=1)
endif()

add_subdirectory(filter_mls)
if (TARGET MeshLab2PluginFilterMLS)
    target_link_libraries(MeshLab2Plugins PUBLIC MeshLab2PluginFilterMLS)
    target_compile_definitions(MeshLab2Plugins PRIVATE MESHLAB2_PLUGIN_FILTER_MLS_ENABLED=1)
endif()

add_subdirectory(filter_vertex_displacement)
if (TARGET MeshLab2PluginFilterVertexDisplacement)
    target_link_libraries(MeshLab2Plugins PUBLIC MeshLab2PluginFilterVertexDisplacement)
    target_compile_definitions(MeshLab2Plugins PRIVATE MESHLAB2_PLUGIN_FILTER_VERTEX_DISPLACEMENT_ENABLED=1)
endif()

add_subdirectory(filter_layer)
if (TARGET MeshLab2PluginFilterLayer)
    target_link_libraries(MeshLab2Plugins PUBLIC MeshLab2PluginFilterLayer)
    target_compile_definitions(MeshLab2Plugins PRIVATE MESHLAB2_PLUGIN_FILTER_LAYER_ENABLED=1)
endif()

add_subdirectory(filter_colorproc)
if (TARGET MeshLab2PluginFilterColorProc)
    target_link_libraries(MeshLab2Plugins PUBLIC MeshLab2PluginFilterColorProc)
    target_compile_definitions(MeshLab2Plugins PRIVATE MESHLAB2_PLUGIN_FILTER_COLORPROC_ENABLED=1)
endif()

add_subdirectory(filter_isoparam)
add_subdirectory(filter_bpa)
add_subdirectory(filter_xatlas)
if (TARGET MeshLab2PluginFilterIsoParam)
    target_link_libraries(MeshLab2Plugins PUBLIC MeshLab2PluginFilterIsoParam)
    target_compile_definitions(MeshLab2Plugins PRIVATE MESHLAB2_PLUGIN_FILTER_ISOPARAM_ENABLED=1)
endif()

if (TARGET MeshLab2PluginFilterBpa)
    target_link_libraries(MeshLab2Plugins PUBLIC MeshLab2PluginFilterBpa)
    target_compile_definitions(MeshLab2Plugins PRIVATE MESHLAB2_PLUGIN_FILTER_BPA_ENABLED=1)
endif()

if (TARGET MeshLab2PluginFilterXAtlas)
    target_link_libraries(MeshLab2Plugins PUBLIC MeshLab2PluginFilterXAtlas)
    target_compile_definitions(MeshLab2Plugins PRIVATE MESHLAB2_PLUGIN_FILTER_XATLAS_ENABLED=1)
endif()

add_subdirectory(filter_trioptimize)
if (TARGET MeshLab2PluginFilterTriOptimize)
    target_link_libraries(MeshLab2Plugins PUBLIC MeshLab2PluginFilterTriOptimize)
    target_compile_definitions(MeshLab2Plugins PRIVATE MESHLAB2_PLUGIN_FILTER_TRIOPTIMIZE_ENABLED=1)
endif()

add_subdirectory(filter_color_projection)
if (TARGET MeshLab2PluginFilterColorProjection)
    target_link_libraries(MeshLab2Plugins PUBLIC MeshLab2PluginFilterColorProjection)
    target_compile_definitions(MeshLab2Plugins PRIVATE MESHLAB2_PLUGIN_FILTER_COLOR_PROJECTION_ENABLED=1)
endif()

add_subdirectory(filter_camera)
if (TARGET MeshLab2PluginFilterCamera)
    target_link_libraries(MeshLab2Plugins PUBLIC MeshLab2PluginFilterCamera)
    target_compile_definitions(MeshLab2Plugins PRIVATE MESHLAB2_PLUGIN_FILTER_CAMERA_ENABLED=1)
endif()

add_subdirectory(filter_img_patch_param)
if (TARGET MeshLab2PluginFilterImgPatchParam)
    target_link_libraries(MeshLab2Plugins PUBLIC MeshLab2PluginFilterImgPatchParam)
    target_compile_definitions(MeshLab2Plugins PRIVATE MESHLAB2_PLUGIN_FILTER_IMG_PATCH_PARAM_ENABLED=1)
endif()

add_subdirectory(filter_plymc)
if (TARGET MeshLab2PluginFilterPlyMC)
    target_link_libraries(MeshLab2Plugins PUBLIC MeshLab2PluginFilterPlyMC)
    target_compile_definitions(MeshLab2Plugins PRIVATE MESHLAB2_PLUGIN_FILTER_PLYMC_ENABLED=1)
endif()
