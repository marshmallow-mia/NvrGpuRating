# NvrGpuRating.dll -- a nEVR plugin that tells Echo VR what GPU it runs on where Proton
# can't (cores and base clock, AMD's TFLOPS), so it picks the right graphics preset. See gpu_rating.cpp.
#
#   cmake -S . -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-toolchain.cmake
#   cmake --build build

cmake_minimum_required(VERSION 3.20)
project(NvrGpuRating VERSION 1.0.0 LANGUAGES C CXX)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# MinHook, pinned as in NvrAssetPatches: the v1.3.4 release tarball, checked by hash.
include(FetchContent)
FetchContent_Declare(minhook
    URL https://github.com/TsudaKageyu/minhook/archive/refs/tags/v1.3.4.tar.gz
    URL_HASH SHA256=1aebeae4ca898330c507860acc2fca2eb335fe446a3a2b8444c3bf8b2660a14e
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR no-cmake-project
)
FetchContent_MakeAvailable(minhook)

add_library(minhook STATIC
    ${minhook_SOURCE_DIR}/src/buffer.c
    ${minhook_SOURCE_DIR}/src/hook.c
    ${minhook_SOURCE_DIR}/src/trampoline.c
    ${minhook_SOURCE_DIR}/src/hde/hde64.c
)
target_include_directories(minhook PUBLIC ${minhook_SOURCE_DIR}/include)

add_library(gpu_rating SHARED gpu_rating.cpp nevr_plugin_interface.h NvrGpuRating.def)
target_include_directories(gpu_rating PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(gpu_rating PRIVATE minhook dxguid)
target_compile_options(gpu_rating PRIVATE -Wall -Wextra)
set_target_properties(gpu_rating PROPERTIES OUTPUT_NAME "NvrGpuRating" PREFIX "")
target_link_options(gpu_rating PRIVATE -Wl,--exclude-all-symbols)
