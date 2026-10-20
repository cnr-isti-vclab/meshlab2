# Overlay of vcpkg's rapidobj port (baseline in vcpkg.json), used by the Linux job only.
# Two changes from the original: "supports" no longer excludes Linux, and the
# pkg-config check is skipped. See the comment above vcpkg_fixup_pkgconfig below.

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO guybrush77/rapidobj
    REF "v${VERSION}"
    SHA512 eddd03556348e44de60af8bd15b5f614ed2588e7c26e0b57cc436f65394d579f379b991126e6542374cfd4f2e110b305ce19135605adc60609448beb53e03d53
    HEAD_REF master
    PATCHES
        fix-build.patch
)

set(VCPKG_BUILD_TYPE release) # header-only port
vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
)

vcpkg_cmake_install()

vcpkg_cmake_config_fixup()

# SKIP_CHECK: the check runs `pkg-config --exists RapidObj` and fails on Linux even
# though the file is installed; nothing here consumes it, MeshLab uses the CMake config.
vcpkg_fixup_pkgconfig(SKIP_CHECK)

file(
    REMOVE_RECURSE
    ${CURRENT_PACKAGES_DIR}/debug
    ${CURRENT_PACKAGES_DIR}/lib
)

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
