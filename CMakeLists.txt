cmake_minimum_required(VERSION 3.16)
project(d_opensubdiv CXX C)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ---- Vendored OpenSubdiv (extern/OpenSubdiv submodule) ---------------------
# CPU-only build — strip everything we don't need (GL/CUDA/CLEW examples,
# Ptex, OpenMP/TBB/OpenCL backends, regression suite) so the build is
# small and dependency-free. Apps that want GPU evaluation can flip
# these on later.

set(NO_OPENGL             OFF CACHE BOOL "" FORCE)
set(NO_OPENCL             ON  CACHE BOOL "" FORCE)
set(NO_CUDA               ON  CACHE BOOL "" FORCE)
set(NO_PTEX               ON  CACHE BOOL "" FORCE)
set(NO_OMP                ON  CACHE BOOL "" FORCE)
set(NO_TBB                ON  CACHE BOOL "" FORCE)
set(NO_DX                 ON  CACHE BOOL "" FORCE)
set(NO_METAL              ON  CACHE BOOL "" FORCE)
set(NO_TESTS              ON  CACHE BOOL "" FORCE)
set(NO_EXAMPLES           ON  CACHE BOOL "" FORCE)
set(NO_TUTORIALS          ON  CACHE BOOL "" FORCE)
set(NO_REGRESSION         ON  CACHE BOOL "" FORCE)
set(NO_DOC                ON  CACHE BOOL "" FORCE)
set(NO_GLEW               ON  CACHE BOOL "" FORCE)
set(NO_GLFW               ON  CACHE BOOL "" FORCE)
set(NO_GLFW_X11           ON  CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS     OFF CACHE BOOL "" FORCE)

# OpenSubdiv's top-level CMakeLists.txt unconditionally registers an
# install(EXPORT opensubdiv-targets …) rule. With every GPU / CLEW /
# OpenMP backend disabled above, the export set ends up empty and
# CMake's generate step errors out. We never install anyway (this
# build is purely a build-time dependency of our shim), so shadow
# `install` with a no-op for the duration of the add_subdirectory
# call — sidesteps the empty-export trap without patching upstream.
function(install)
endfunction()

# Apple Clang 21+ (macOS 26 Tahoe): libc++ top-level headers (cassert,
# memory, …) moved into the SDK; CLT's usr/include/c++/v1 only carries
# internal __xxx files. Inject the SDK path so the compiler finds them.
if(APPLE AND CMAKE_OSX_SYSROOT)
    add_compile_options(-isystem ${CMAKE_OSX_SYSROOT}/usr/include/c++/v1)
endif()

add_subdirectory(extern/OpenSubdiv EXCLUDE_FROM_ALL)
# Restore install() — caller of this top-level project may want it.
# (CMake's function shadowing is scope-local, so this just clears the
# wrapper from the current directory's symbol table.)

# ---- The shim --------------------------------------------------------------
add_library(osdc STATIC csrc/osd_c.cpp)

target_include_directories(osdc
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/extern/OpenSubdiv
)

# OpenSubdiv exposes both CPU and Far sub-libs from its add_subdirectory.
# `osd_static_cpu` already transitively links `osd_static_far`, so a
# single target_link_libraries entry covers both.
target_link_libraries(osdc PUBLIC osd_static_cpu osd_static_gpu)

# Drop the typical warnings that OpenSubdiv's older C++14 code trips on.
if(MSVC)
    target_compile_options(osdc PRIVATE /W3 /wd4996 /wd4244 /wd4267)
else()
    target_compile_options(osdc PRIVATE -Wall -Wno-deprecated-declarations)
endif()
