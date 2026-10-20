# Release-only Linux triplet. The builtin x64-linux also builds every dependency in
# debug, which doubles the build time and the size of the binary cache for a
# configuration CI never uses. Static libraries, as in the builtin triplet: fewer
# shared objects to bundle when this is packaged as an AppImage.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME Linux)
set(VCPKG_BUILD_TYPE release)
