set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME Darwin)
set(VCPKG_OSX_ARCHITECTURES arm64)

# 修复 install_name_tool RPATH 空间不足警告（libpq pg_config 等）
set(VCPKG_LINKER_FLAGS "-Wl,-headerpad_max_install_names")
