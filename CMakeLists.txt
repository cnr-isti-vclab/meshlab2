cmake_minimum_required(VERSION 3.25)

# Must precede project(): otherwise the minimum macOS version of the shipped
# binaries silently becomes whatever the build machine happens to run. Apple
# Silicon only, so the floor is 15.0 rather than the 13.0 Qt would allow.
# Tested for emptiness, not DEFINED: existing build trees already carry an empty
# cache entry, which a plain CACHE set() would refuse to overwrite. An explicit
# -DCMAKE_OSX_DEPLOYMENT_TARGET still wins.
if (APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET 15.0)
endif()

project(MeshLab2 LANGUAGES CXX)

# geogram is the first vcpkg dependency that is a shared library (its port
# forces dynamic linkage on Darwin), and its install name is @rpath-relative.
# CMake does not add the vcpkg lib directory to the build rpath by itself
# here, so every executable -- the app and all fourteen test binaries --
# would link fine and then die at launch with a dyld "Library not loaded"
# error. Must precede the first target: CMAKE_BUILD_RPATH is read when a
# target is created, not when it is linked.
if (VCPKG_INSTALLED_DIR AND VCPKG_TARGET_TRIPLET)
    list(APPEND CMAKE_BUILD_RPATH "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib")

    # geogram's spectral methods -- spectral LSCM, the spectral chart parametrizer,
    # the manifold-harmonic segmenters -- reach their eigensolver through OpenNL's
    # ARPACK extension, which dlopen()s "libarpack" by leaf name. No vcpkg port
    # installs that name, but nothing has to: OpenNL passes NL_LINK_USE_FALLBACK and
    # retries with "libgeogram_num_3rdparty", the second library the geogram port
    # installs, which carries geogram's vendored ARPACK and exports exactly the four
    # symbols it looks up. That file sits in the vcpkg lib directory already added to
    # the rpath above, so the spectral paths work with no further help here.
    #
    # It does mean the bundle must ship BOTH geogram dylibs even though nothing links
    # the second one -- see scripts/package-macos-dmg.sh.
endif()

# MeshLab2's own root, used by every path below and inherited by plugins/ and
# src/. Deliberately not CMAKE_SOURCE_DIR: pymeshlab2 pulls this tree in with
# add_subdirectory(), where that variable is the *outer* project's root and
# every path built from it silently points at a directory we do not own.
set(MESHLAB2_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR})

# Product identity, single source of truth for the macOS bundle (see
# resources/Info.plist.in) and anywhere else the app names itself.
# Version follows the MeshLab date scheme (YYYY.MM); CI may override it.
set(MESHLAB2_VERSION "2026.11" CACHE STRING "Product version, YYYY.MM")
set(MESHLAB2_BUNDLE_NAME "MeshLab")
# Deliberately distinct from classic MeshLab's com.vcg.meshlab so both can be
# installed side by side. Changing this invalidates code-signing identity and
# the user's TCC privacy grants, so treat it as stable.
set(MESHLAB2_BUNDLE_IDENTIFIER "net.meshlab.MeshLab")
set(MESHLAB2_BUNDLE_COPYRIGHT "© 2026 Visual Computing Lab, CNR-ISTI. GPL-3.0.")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(QT_NO_PRIVATE_MODULE_WARNING ON)

option(MESHLAB2_REQUIRE_VCPKG_DEPS "Require vcpkg toolchain for non-Qt external dependencies" ON)
option(MESHLAB2_MATH_HELP "Render LaTeX formulas in filter help" ON)
if (MESHLAB2_REQUIRE_VCPKG_DEPS)
    if (NOT DEFINED VCPKG_TARGET_TRIPLET)
        message(FATAL_ERROR
            "MESHLAB2_REQUIRE_VCPKG_DEPS=ON but vcpkg toolchain is not active. "
            "Set VCPKG_ROOT to your vcpkg clone and configure with "
            "-DCMAKE_TOOLCHAIN_FILE=$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake, "
            "or use the 'vcpkg-manifest' preset.")
    endif()
    message(STATUS "MeshLab external dependency mode: vcpkg (triplet: ${VCPKG_TARGET_TRIPLET})")
endif()

find_package(Qt6 6.11 REQUIRED COMPONENTS Widgets GuiPrivate ShaderTools Test)

if(MESHLAB2_MATH_HELP)
    find_package(Qt6 6.11 REQUIRED COMPONENTS Xml)
    if(NOT EXISTS "${MESHLAB2_SOURCE_DIR}/external/jkqtplotter/CMakeLists.txt")
        message(FATAL_ERROR
            "MESHLAB2_MATH_HELP=ON but external/jkqtplotter is missing. "
            "Run: git submodule update --init external/jkqtplotter")
    endif()
    set(JKQtPlotter_BUILD_INCLUDE_XITS_FONTS OFF)
    set(JKQtPlotter_BUILD_INCLUDE_FIRAMATH_FONTS ON)
    set(JKQtPlotter_BUILD_LIB_JKQTMATH OFF)
    set(JKQtPlotter_BUILD_LIB_JKQTMATHTEXT ON)
    set(JKQtPlotter_BUILD_LIB_JKQTFASTPLOTTER OFF)
    set(JKQtPlotter_BUILD_LIB_JKQTPLOTTER OFF)
    set(JKQtPlotter_BUILD_FORCE_NO_PRINTER_SUPPORT ON)
    set(JKQtPlotter_BUILD_WITH_PRECOMPILED_HEADERS OFF)
    if(DEFINED BUILD_SHARED_LIBS)
        set(_meshlab2_build_shared_libs "${BUILD_SHARED_LIBS}")
    endif()
    set(BUILD_SHARED_LIBS OFF)
    add_subdirectory(external/jkqtplotter EXCLUDE_FROM_ALL)
    if(DEFINED _meshlab2_build_shared_libs)
        set(BUILD_SHARED_LIBS "${_meshlab2_build_shared_libs}")
        unset(_meshlab2_build_shared_libs)
    else()
        unset(BUILD_SHARED_LIBS)
    endif()
endif()

# ---------------------------------------------------------------------------
# Optional Intel Embree ray casting (shared by core + filter_embree)
# ---------------------------------------------------------------------------
include(${MESHLAB2_SOURCE_DIR}/cmake/MeshLab2Embree.cmake)

# ---------------------------------------------------------------------------
# Optional embedded Python console
# ---------------------------------------------------------------------------
option(MESHLAB2_PYTHON_CONSOLE "Build embedded Python interpreter and console dock" ON)
if(MESHLAB2_PYTHON_CONSOLE)
    # nanobind's cmake config requires the CMake `Python` package (not `Python3`)
    # because it checks for the Python::Module imported target.
    find_package(Python REQUIRED COMPONENTS Interpreter Development.Embed Development.Module)
    # nanobind is provided by the local vcpkg ports (vcpkg/ports/nanobind).
    find_package(nanobind CONFIG REQUIRED)
    message(STATUS "MeshLab: Python console enabled (Python ${Python_VERSION})")
endif()

add_library(MeshLab2Core STATIC
    src/core/camerashot.h src/core/camerashot.cpp
    src/core/document.h src/core/document.cpp
    src/core/document_internal.h src/core/document_internal.cpp
    src/core/documentundomanager.h src/core/documentundomanager.cpp
    src/core/document_filters.cpp
    src/core/document_gpu.cpp
    src/core/document_io.cpp
    src/core/document_layers.cpp
    src/core/document_logging.cpp
    src/core/document_memory.cpp
    src/core/document_project.cpp
    src/core/document_textures.cpp
    src/core/document_undo.cpp
    src/core/document_undo_state.cpp
    src/core/headlessrendercontext.h src/core/headlessrendercontext.cpp
    src/core/helperprocess.h src/core/helperprocess.cpp
    src/core/memorypressuremonitor.h src/core/memorypressuremonitor.cpp
    src/core/filterparam.h src/core/filterparam.cpp
    src/core/filedialogdirectory.h src/core/filedialogdirectory.cpp
    src/core/textureassociationutils.h src/core/textureassociationutils.cpp
    src/core/stbimageimpl.cpp
    src/core/uvpacker.h src/core/uvpacker.cpp
    src/core/viewpointoccluder.h src/core/viewpointoccluder.cpp
    src/core/layerdata.h
    src/core/vcgmesh.h
    src/plugins/filtercategories.h src/plugins/filtercategories.cpp
    src/core/preferences.h src/core/preferences.cpp
    src/core/processmemoryinfo.h src/core/processmemoryinfo.cpp
    src/plugins/filterdescriptorloader.h src/plugins/filterdescriptorloader.cpp
    src/plugins/meshfilterplugin.h src/plugins/meshfilterplugin.cpp
    src/plugins/meshfilterpluginmanager.h src/plugins/meshfilterpluginmanager.cpp
    src/plugins/meshioplugin.h
    src/plugins/meshiopluginmanager.h src/plugins/meshiopluginmanager.cpp
    src/render/colormap.h src/render/colormap.cpp
    src/render/linerenderer.h src/render/linerenderer.cpp
    src/render/meshgpuresourcecache.h src/render/meshgpuresourcecache.cpp
    src/render/renderingsettings.h
    src/render/rendersettingsjson.h src/render/rendersettingsjson.cpp
    src/render/viewstate.h
    src/render/viewgridlayout.h src/render/viewgridlayout.cpp
    src/render/viewfrustumgizmo.h src/render/viewfrustumgizmo.cpp
    src/render/clipplane.h src/render/clipplane.cpp
    src/render/viewsplitterlayout.h src/render/viewsplitterlayout.cpp
    src/render/viewaxisgizmo.h src/render/viewaxisgizmo.cpp
    src/render/viewtrackball.h src/render/viewtrackball.cpp
)

# Backs the rasterized packers in uvpacker; the only .cpp vcglib needs compiled in.
target_sources(MeshLab2Core PRIVATE ${MESHLAB2_SOURCE_DIR}/vcglib/wrap/qt/outline2_rasterizer.cpp)

# stb_image backs readImageFile's fallback for the formats Qt declines -- notably any
# Targa without the TrueVision 2.0 footer, which QTgaHandler rejects outright. The
# implementation is compiled here, once, and stbimageimpl.cpp is the only translation
# unit that may define STB_IMAGE_IMPLEMENTATION: tinygltf pulls the same header into
# the glTF plugin, so a second definition would collide at link time.
find_path(STB_INCLUDE_DIR stb_image.h PATH_SUFFIXES stb)
if (STB_INCLUDE_DIR)
    target_include_directories(MeshLab2Core PUBLIC ${STB_INCLUDE_DIR})
    target_compile_definitions(MeshLab2Core PUBLIC MESHLAB2_HAS_STB_IMAGE)
else()
    message(WARNING "stb_image.h not found: images Qt cannot decode (legacy .tga) will fail to load.")
endif()

target_include_directories(MeshLab2Core PUBLIC
    ${MESHLAB2_SOURCE_DIR}/src/core
    ${MESHLAB2_SOURCE_DIR}/src/render
    ${MESHLAB2_SOURCE_DIR}/src/plugins
    ${MESHLAB2_SOURCE_DIR}
    ${MESHLAB2_SOURCE_DIR}/vcglib
    ${MESHLAB2_SOURCE_DIR}/vcglib/eigenlib
)

target_link_libraries(MeshLab2Core PUBLIC
    Qt6::Core
    Qt6::Gui
    Qt6::GuiPrivate
    Qt6::Widgets
)

if(WIN32)
    # Prevent the Windows SDK's min/max macros from rewriting std::min and
    # std::max in vcglib, CGAL, and plugins that consume the core target.
    target_compile_definitions(MeshLab2Core PUBLIC NOMINMAX)
    target_link_libraries(MeshLab2Core PRIVATE Psapi)
endif()

# Embree occlusion backend (no-op interface target when embree is unavailable).
target_link_libraries(MeshLab2Core PRIVATE MeshLab2Embree)

qt_add_resources(MeshLab2Core "preferences"
    PREFIX "/"
    FILES
        resources/preferences.json
)

qt_add_resources(MeshLab2Core "colormaps"
    PREFIX "/"
    FILES
        colormaps/rainbow.json
        colormaps/viridis.json
        colormaps/gray.json
        colormaps/plasma.json
        colormaps/cividis.json
        colormaps/turbo.json
)

add_subdirectory(plugins)

if(MESHLAB2_PYTHON_CONSOLE)
    add_subdirectory(src/python)
endif()

add_library(MeshLab2MathHelp STATIC
    src/ui/mathmarkdownrenderer.h
    src/ui/mathmarkdownrenderer.cpp
)
target_link_libraries(MeshLab2MathHelp PUBLIC Qt6::Widgets)
target_include_directories(MeshLab2MathHelp PUBLIC ${MESHLAB2_SOURCE_DIR}/src/ui)
if(MESHLAB2_MATH_HELP)
    target_link_libraries(MeshLab2MathHelp PRIVATE JKQTPlotter6::JKQTMathText6)
    target_compile_definitions(MeshLab2MathHelp PRIVATE MESHLAB2_MATH_HELP)
endif()

qt_add_executable(MeshLab2
    src/app/main.cpp
    src/ui/aboutdialog.h src/ui/aboutdialog.cpp
    src/ui/mainwindow.h src/ui/mainwindow.cpp
    src/ui/meshfilterpanel.h src/ui/meshfilterpanel.cpp
    src/ui/parameterformbuilder.h src/ui/parameterformbuilder.cpp
    src/ui/preferencesdialog.h src/ui/preferencesdialog.cpp
    src/ui/filterpresentation.h src/ui/filterpresentation.cpp
    src/ui/filterpluginsinfodialog.h src/ui/filterpluginsinfodialog.cpp
    src/ui/meshsaveoptionsdialog.h src/ui/meshsaveoptionsdialog.cpp
    src/ui/snapshotdialog.h src/ui/snapshotdialog.cpp
    src/render/renderwidget.h src/render/renderwidget.cpp
    src/render/renderwidget_internal.h
    src/render/renderwidget_modes.cpp
    src/render/renderwidget_resources.cpp
    src/render/renderwidget_selection.cpp
    src/render/renderwidget_frame_plan.cpp
    src/render/renderwidget_scene_passes.cpp
    src/render/renderwidget_fill.cpp
    src/render/renderwidget_render.cpp
    src/render/renderwidget_raster.cpp
    src/render/renderwidget_uv.cpp
    src/render/viewtrackball.h
    src/render/viewstate.h
    src/render/renderoverlaypanel.h src/render/renderoverlaypanel.cpp
    src/render/interactivetool.h src/render/interactivetoolfactory.cpp
    src/render/tools/selectlayertool.h src/render/tools/selectlayertool.cpp
    src/render/tools/rubberbandselecttool.h src/render/tools/rubberbandselecttool.cpp
    src/render/tools/measuretool.h src/render/tools/measuretool.cpp
    src/render/tools/transformtool.h src/render/tools/transformtool.cpp
    src/ui/layerwidget.h src/ui/layerwidget.cpp
    src/ui/undographwidget.h src/ui/undographwidget.cpp
)

# The target is MeshLab2 (following the repository); the product is MeshLab.
set_target_properties(MeshLab2 PROPERTIES
    OUTPUT_NAME "MeshLab"
    MACOSX_BUNDLE TRUE
    # Also set as target properties, not only in the plist: the Xcode generator and
    # some Qt deployment tooling read these rather than the generated Info.plist.
    MACOSX_BUNDLE_GUI_IDENTIFIER "${MESHLAB2_BUNDLE_IDENTIFIER}"
    MACOSX_BUNDLE_BUNDLE_NAME "${MESHLAB2_BUNDLE_NAME}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${MESHLAB2_VERSION}"
    MACOSX_BUNDLE_BUNDLE_VERSION "${MESHLAB2_VERSION}"
)

option(MESHLAB2_MACOS_BUNDLE_ICON "Generate and embed the macOS app bundle icon" ON)
if (APPLE AND MESHLAB2_MACOS_BUNDLE_ICON)
    set(MESHLAB2_APP_ICON_PNG "${MESHLAB2_SOURCE_DIR}/img/MeshLab_Icon_512x512.png")
    set(MESHLAB2_APP_ICON_ICNS "${CMAKE_CURRENT_BINARY_DIR}/MeshLab.icns")
    add_custom_command(
        OUTPUT "${MESHLAB2_APP_ICON_ICNS}"
        COMMAND "${CMAKE_COMMAND}"
            -DINPUT_PNG=${MESHLAB2_APP_ICON_PNG}
            -DOUTPUT_ICNS=${MESHLAB2_APP_ICON_ICNS}
            -P "${MESHLAB2_SOURCE_DIR}/cmake/generate_mac_icon.cmake"
        DEPENDS "${MESHLAB2_APP_ICON_PNG}" "${MESHLAB2_SOURCE_DIR}/cmake/generate_mac_icon.cmake"
        COMMENT "Generating macOS app icon"
        VERBATIM
    )
    target_sources(MeshLab2 PRIVATE "${MESHLAB2_APP_ICON_ICNS}")
    set_source_files_properties("${MESHLAB2_APP_ICON_ICNS}" PROPERTIES
        MACOSX_PACKAGE_LOCATION "Resources"
    )
    set(MESHLAB2_BUNDLE_ICON_NAME "MeshLab.icns")
    set_target_properties(MeshLab2 PROPERTIES
        MACOSX_BUNDLE_ICON_FILE "${MESHLAB2_BUNDLE_ICON_NAME}"
    )
endif()

# macOS bundle metadata: identity, version, minimum OS, and the document type
# associations that make double-click open, "Open With", and drag-onto-Dock work.
# Qt's default template supplies none of that — it ships a placeholder identifier,
# reports version 1.0, leaves LSMinimumSystemVersion empty, and registers no
# document types at all. Pre-configured here with @ONLY so every value is
# resolved from this scope rather than by Qt's own substitution pass.
if (APPLE)
    if (NOT DEFINED MESHLAB2_BUNDLE_ICON_NAME)
        set(MESHLAB2_BUNDLE_ICON_NAME "")
    endif()
    configure_file(
        "${MESHLAB2_SOURCE_DIR}/resources/Info.plist.in"
        "${CMAKE_CURRENT_BINARY_DIR}/Info.plist"
        @ONLY
    )
    set_target_properties(MeshLab2 PROPERTIES
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_BINARY_DIR}/Info.plist"
    )
endif()

target_include_directories(MeshLab2 PRIVATE
    ${MESHLAB2_SOURCE_DIR}/src/ui
)

foreach(MESHLAB2_GIT_STATE IN ITEMS .git/HEAD .git/logs/HEAD)
    if(EXISTS "${MESHLAB2_SOURCE_DIR}/${MESHLAB2_GIT_STATE}")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
            "${MESHLAB2_SOURCE_DIR}/${MESHLAB2_GIT_STATE}")
    endif()
endforeach()
execute_process(
    COMMAND git -C "${MESHLAB2_SOURCE_DIR}" rev-parse --short=12 HEAD
    OUTPUT_VARIABLE MESHLAB2_BUILD_ID
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT MESHLAB2_BUILD_ID)
    set(MESHLAB2_BUILD_ID "unversioned")
endif()
target_compile_definitions(MeshLab2 PRIVATE MESHLAB2_BUILD_ID="${MESHLAB2_BUILD_ID}")

target_link_libraries(MeshLab2 PRIVATE
    MeshLab2Plugins
    MeshLab2MathHelp
    Qt6::Widgets
    Qt6::GuiPrivate
)

if(COMMAND meshlab2_bundle_quadwild)
    meshlab2_bundle_quadwild(MeshLab2)
endif()

if(MESHLAB2_PYTHON_CONSOLE)
    target_sources(MeshLab2 PRIVATE
        src/ui/pythonconsole.h
        src/ui/pythonconsole.cpp
        src/ui/scripteditor.h
        src/ui/scripteditor.cpp
    )
    target_link_libraries(MeshLab2 PRIVATE MeshLab2PythonHost _meshlab2_bindings)
    target_compile_definitions(MeshLab2 PRIVATE MESHLAB2_PYTHON_CONSOLE)
    # Embed the Python lib dir in the rpath so the app finds libpython at
    # runtime regardless of where conda / homebrew installed it.
    set_property(TARGET MeshLab2 APPEND PROPERTY
        BUILD_RPATH "${Python_LIBRARY_DIRS}")
    set_property(TARGET MeshLab2 APPEND PROPERTY
        INSTALL_RPATH "${Python_LIBRARY_DIRS}")
endif()

qt_add_shaders(MeshLab2 "shaders"
    PREFIX "/"
    # Qt would otherwise also bake GLSL 100 es and 120. Neither can express
    # gl_ClipDistance -- SPIRV-Cross emits a global `out` that those profiles reject --
    # and qsb does not report it, so the breakage would only appear when the OpenGL
    # backend compiled the shader at run time. The clipping plane needs clip distances,
    # so the OpenGL floor is 3.2 core. Metal, D3D11 and Vulkan are unaffected.
    GLSL "150"
    FILES
        shaders/fill_smooth.vert
        shaders/fill_smooth.frag
        shaders/fill_flat.vert
        shaders/fill_flat.frag
        shaders/fill_radscale.frag
        shaders/fill_radscale_buf.frag
        shaders/fill_wire.vert
        shaders/fill_wire_overlay.frag
        shaders/overlay_bbox.vert
        shaders/overlay_bbox.frag
        shaders/overlay_edges.vert
        shaders/overlay_edges.frag
        shaders/overlay_fat_edges.vert
        shaders/overlay_fat_edges.frag
        shaders/overlay_fat_decorator.vert
        shaders/overlay_fat_decorator.frag
        shaders/overlay_points.vert
        shaders/overlay_points.frag
        shaders/overlay_decorator.vert
        shaders/overlay_decorator.frag
        shaders/overlay_trackball_gizmo.vert
        shaders/overlay_trackball_gizmo.frag
        shaders/overlay_light_gizmo.vert
        shaders/overlay_light_gizmo.frag
        shaders/selection_mask.vert
        shaders/selection_mask.frag
        shaders/depth_pick.vert
        shaders/depth_pick.frag
        shaders/selection_mask_morph.frag
        shaders/selection_mask_debug.frag
        shaders/selection_outline.vert
        shaders/selection_outline_extract.frag
        shaders/selection_outline.frag
        shaders/scene_background.vert
        shaders/scene_background.frag
        shaders/raster_backplate.vert
        shaders/raster_backplate.frag
        shaders/raster_projected.vert
        shaders/raster_projected.frag
        shaders/uv_background.vert
        shaders/uv_background.frag
        shaders/uv_fill_texture.vert
)

qt_add_resources(MeshLab2 "icons"
    PREFIX "/"
    FILES
        img/options.png
        img/global.png
        img/normals.png
        img/boundary.png
        img/box.png
        img/clipplane.png
        img/points.png
        img/edge-mesh.png
        img/wireframe.png
        img/wire.png
        img/flat.png
        img/selected.png
        img/sel_all.png
        img/sel_none.png
        img/sel_plus.png
        img/sel_minus.png
        img/sel_inv.png
        img/delete_vert.png
        img/delete_face.png
        img/delete_facevert.png
        img/histogram.png
        img/splitH.png
        img/splitV.png
        img/MeshLab_Icon_512x512.png
        img/layer_eye_open.png
        img/layer_eye_close.png
        img/tool_select_layer.png
        img/tool_select_rect.png
        img/icon_measure.png
        img/snapshot.png
        img/axis.png
        img/cur_pick.png
        img/cur_measure.png
        img/cur_trackball.png
        img/cur_sel_rect.png
        img/cur_sel_rect_plus.png
        img/cur_sel_rect_minus.png
        img/cur_sel_rect_eye.png
        img/cur_sel_rect_plus_eye.png
        img/cur_sel_rect_minus_eye.png
        img/cur_badge_cc.png
)

qt_add_resources(MeshLab2 "help"
    PREFIX "/"
    FILES
        resources/quick_help_overlay.html
)

enable_testing()

qt_add_executable(MeshLab2Tests
    tests/test_document.cpp
)

target_include_directories(MeshLab2Tests PRIVATE
    ${MESHLAB2_SOURCE_DIR}
)

target_link_libraries(MeshLab2Tests PRIVATE
    MeshLab2Plugins
    Qt6::Test
)

target_compile_definitions(MeshLab2Tests PRIVATE
    TEST_SOURCE_DIR="${MESHLAB2_SOURCE_DIR}"
)

add_test(NAME MeshLab2Tests COMMAND MeshLab2Tests)

qt_add_executable(MeshLab2ColorMapTests
    tests/test_colormap.cpp
)

target_include_directories(MeshLab2ColorMapTests PRIVATE
    ${MESHLAB2_SOURCE_DIR}
)

target_link_libraries(MeshLab2ColorMapTests PRIVATE
    MeshLab2Core
    Qt6::Test
)

add_test(NAME MeshLab2ColorMapTests COMMAND MeshLab2ColorMapTests)

if(MESHLAB2_MATH_HELP)
    qt_add_executable(MeshLab2MathMarkdownTests
        tests/test_mathmarkdown.cpp
    )
    target_link_libraries(MeshLab2MathMarkdownTests PRIVATE
        MeshLab2MathHelp
        Qt6::Test
    )
    add_test(NAME MeshLab2MathMarkdownTests COMMAND MeshLab2MathMarkdownTests)
endif()

qt_add_executable(MeshLab2FilterTests
    tests/test_filters.cpp
)

target_include_directories(MeshLab2FilterTests PRIVATE
    ${MESHLAB2_SOURCE_DIR}
    # clipplane.h: the plane the render panel's Trim button hands to the filter is resolved
    # by the same code the viewport uses, so the test can check the two agree.
    ${MESHLAB2_SOURCE_DIR}/src/render
)

target_link_libraries(MeshLab2FilterTests PRIVATE
    MeshLab2Plugins
    Qt6::Test
)

target_compile_definitions(MeshLab2FilterTests PRIVATE
    TEST_SOURCE_DIR="${MESHLAB2_SOURCE_DIR}"
)

add_test(NAME MeshLab2FilterTests COMMAND MeshLab2FilterTests)

qt_add_executable(MeshLab2FilterCreationTests
    tests/test_filter_creation.cpp
)

target_include_directories(MeshLab2FilterCreationTests PRIVATE
    ${MESHLAB2_SOURCE_DIR}
)

target_link_libraries(MeshLab2FilterCreationTests PRIVATE
    MeshLab2Plugins
    Qt6::Test
)

target_compile_definitions(MeshLab2FilterCreationTests PRIVATE
    TEST_SOURCE_DIR="${MESHLAB2_SOURCE_DIR}"
)

add_test(NAME MeshLab2FilterCreationTests COMMAND MeshLab2FilterCreationTests)

qt_add_executable(MeshLab2FilterDescriptorTests
    tests/test_filter_descriptors.cpp
)

target_include_directories(MeshLab2FilterDescriptorTests PRIVATE
    ${MESHLAB2_SOURCE_DIR}
)

target_link_libraries(MeshLab2FilterDescriptorTests PRIVATE
    MeshLab2Plugins
    Qt6::Test
)

target_compile_definitions(MeshLab2FilterDescriptorTests PRIVATE
    TEST_SOURCE_DIR="${MESHLAB2_SOURCE_DIR}"
)

add_test(NAME MeshLab2FilterDescriptorTests COMMAND MeshLab2FilterDescriptorTests)

# Reads .gitmodules, vcpkg.json and the vendored trees back against the About dialog's
# Libraries tab, so that list cannot go stale unnoticed. Parses sources as text, so it
# links nothing but QtTest.
qt_add_executable(MeshLab2ThirdPartyTests
    tests/test_third_party.cpp
)

target_link_libraries(MeshLab2ThirdPartyTests PRIVATE
    Qt6::Test
)

target_compile_definitions(MeshLab2ThirdPartyTests PRIVATE
    TEST_SOURCE_DIR="${MESHLAB2_SOURCE_DIR}"
)

add_test(NAME MeshLab2ThirdPartyTests COMMAND MeshLab2ThirdPartyTests)

qt_add_executable(MeshLab2FilterSmokeTests
    tests/test_filter_smoke.cpp
)

target_include_directories(MeshLab2FilterSmokeTests PRIVATE
    ${MESHLAB2_SOURCE_DIR}
)

target_link_libraries(MeshLab2FilterSmokeTests PRIVATE
    MeshLab2Plugins
    Qt6::Test
)

target_compile_definitions(MeshLab2FilterSmokeTests PRIVATE
    TEST_SOURCE_DIR="${MESHLAB2_SOURCE_DIR}"
)

add_test(NAME MeshLab2FilterSmokeTests COMMAND MeshLab2FilterSmokeTests)

# ParameterFormBuilder deliberately depends on nothing but Document + Qt Widgets,
# so it can be compiled straight into the test rather than dragging in the app.
qt_add_executable(MeshLab2ParameterFormTests
    tests/test_parameterform.cpp
    src/ui/parameterformbuilder.h src/ui/parameterformbuilder.cpp
)

target_include_directories(MeshLab2ParameterFormTests PRIVATE
    ${MESHLAB2_SOURCE_DIR}
    ${MESHLAB2_SOURCE_DIR}/src/ui
)

target_link_libraries(MeshLab2ParameterFormTests PRIVATE
    # The document-coupled editors (the layer pickers) can only be exercised against a
    # real Document, whose constructor registers the built-in plugins -- so this widgets
    # test has to link them even though it never runs a filter.
    MeshLab2Plugins
    MeshLab2Core
    Qt6::Test
    Qt6::Widgets
)

add_test(NAME MeshLab2ParameterFormTests COMMAND MeshLab2ParameterFormTests)

qt_add_executable(MeshLab2PreferencesTests
    tests/test_preferences.cpp
)

target_include_directories(MeshLab2PreferencesTests PRIVATE
    ${MESHLAB2_SOURCE_DIR}
    ${MESHLAB2_SOURCE_DIR}/src/core
)

# MeshLab2Core's Document pulls in the plugin registries, so link the same
# aggregate the other document-backed tests use.
target_link_libraries(MeshLab2PreferencesTests PRIVATE
    MeshLab2Plugins
    Qt6::Test
)

add_test(NAME MeshLab2PreferencesTests COMMAND MeshLab2PreferencesTests)

qt_add_executable(MeshLab2ViewLayoutTests
    tests/test_viewsplitterlayout.cpp
)

target_include_directories(MeshLab2ViewLayoutTests PRIVATE
    ${MESHLAB2_SOURCE_DIR}
    ${MESHLAB2_SOURCE_DIR}/src/render
)

# Splitter surgery only, so this needs Core rather than the whole plugin aggregate.
target_link_libraries(MeshLab2ViewLayoutTests PRIVATE
    MeshLab2Core
    Qt6::Test
    Qt6::Widgets
)

add_test(NAME MeshLab2ViewLayoutTests COMMAND MeshLab2ViewLayoutTests)

qt_add_executable(MeshLab2ViewGridLayoutTests
    tests/test_viewgridlayout.cpp
)

target_include_directories(MeshLab2ViewGridLayoutTests PRIVATE
    ${MESHLAB2_SOURCE_DIR}
    ${MESHLAB2_SOURCE_DIR}/src/render
)

# Tile arithmetic only -- no document, no GPU, not even a widget.
target_link_libraries(MeshLab2ViewGridLayoutTests PRIVATE
    MeshLab2Core
    Qt6::Test
)

add_test(NAME MeshLab2ViewGridLayoutTests COMMAND MeshLab2ViewGridLayoutTests)

qt_add_executable(MeshLab2ViewFrustumGizmoTests
    tests/test_viewfrustumgizmo.cpp
)

target_include_directories(MeshLab2ViewFrustumGizmoTests PRIVATE
    ${MESHLAB2_SOURCE_DIR}
    ${MESHLAB2_SOURCE_DIR}/src/render
)

# Frustum arithmetic only -- no document, no GPU, not even a widget.
target_link_libraries(MeshLab2ViewFrustumGizmoTests PRIVATE
    MeshLab2Core
    Qt6::Test
)

add_test(NAME MeshLab2ViewFrustumGizmoTests COMMAND MeshLab2ViewFrustumGizmoTests)

qt_add_executable(MeshLab2ClipPlaneTests
    tests/test_clipplane.cpp
)

target_include_directories(MeshLab2ClipPlaneTests PRIVATE
    ${MESHLAB2_SOURCE_DIR}
    ${MESHLAB2_SOURCE_DIR}/src/render
)

# Plane arithmetic only -- no document, no GPU, not even a widget.
target_link_libraries(MeshLab2ClipPlaneTests PRIVATE
    MeshLab2Core
    Qt6::Test
)

add_test(NAME MeshLab2ClipPlaneTests COMMAND MeshLab2ClipPlaneTests)

qt_add_executable(MeshLab2LineGeometryTests
    tests/test_linegeometry.cpp
)

target_include_directories(MeshLab2LineGeometryTests PRIVATE
    ${MESHLAB2_SOURCE_DIR}
    ${MESHLAB2_SOURCE_DIR}/src/render
)

# Line geometry only -- no document, no GPU, not even a widget.
target_link_libraries(MeshLab2LineGeometryTests PRIVATE
    MeshLab2Core
    Qt6::Test
)

add_test(NAME MeshLab2LineGeometryTests COMMAND MeshLab2LineGeometryTests)

qt_add_executable(MeshLab2AxisGizmoTests
    tests/test_viewaxisgizmo.cpp
)

target_include_directories(MeshLab2AxisGizmoTests PRIVATE
    ${MESHLAB2_SOURCE_DIR}
    ${MESHLAB2_SOURCE_DIR}/src/render
    ${MESHLAB2_SOURCE_DIR}/src/core
)

# The gizmo reads Preferences, which drags in Document, which references the plugin
# registries -- so this needs the same aggregate the other document-backed tests link.
target_link_libraries(MeshLab2AxisGizmoTests PRIVATE
    MeshLab2Plugins
    Qt6::Test
    Qt6::Widgets
)

add_test(NAME MeshLab2AxisGizmoTests COMMAND MeshLab2AxisGizmoTests)

qt_add_executable(MeshLab2RenderSettingsJsonTests
    tests/test_rendersettingsjson.cpp
)

target_include_directories(MeshLab2RenderSettingsJsonTests PRIVATE
    ${MESHLAB2_SOURCE_DIR}
    ${MESHLAB2_SOURCE_DIR}/src/render
)

target_link_libraries(MeshLab2RenderSettingsJsonTests PRIVATE
    MeshLab2Core
    Qt6::Test
)

add_test(NAME MeshLab2RenderSettingsJsonTests COMMAND MeshLab2RenderSettingsJsonTests)
