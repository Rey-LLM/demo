cmake_minimum_required(VERSION 3.20)
project(ftviz VERSION 1.0.0 LANGUAGES C CXX)

# ----------------------------------------------------------------------------
# Global settings
# ----------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(FTVIZ_BUILD_APP     "Build the OpenGL application (fetches GLFW + glad)" ON)
option(FTVIZ_BUILD_TESTS   "Build unit tests"                                   ON)
option(FTVIZ_BUILD_TOOLS   "Build tools (synthetic data generator)"             ON)
option(FTVIZ_WERROR        "Treat warnings as errors"                           OFF)
option(FTVIZ_NATIVE        "Compile with -march=native (non-portable binaries)" OFF)
option(FTVIZ_GLFW_WAYLAND  "Build GLFW with Wayland support on Linux"           OFF)

find_package(Threads REQUIRED)

# ----------------------------------------------------------------------------
# Dependencies (FetchContent, exact pinned tags)
# ----------------------------------------------------------------------------
include(FetchContent)
set(FETCHCONTENT_QUIET ON)

# xxHash 0.8.3 and GLM 1.0.1 are consumed header-only. SOURCE_SUBDIR points at
# a directory without a CMakeLists.txt so FetchContent only downloads them and
# we do not inherit their install/test targets or their warning flags.
FetchContent_Declare(xxhash
  GIT_REPOSITORY https://github.com/Cyan4973/xxHash.git
  GIT_TAG        v0.8.3
  GIT_SHALLOW    TRUE
  SOURCE_SUBDIR  ftviz_header_only)
FetchContent_Declare(glm
  GIT_REPOSITORY https://github.com/g-truc/glm.git
  GIT_TAG        1.0.1
  GIT_SHALLOW    TRUE
  SOURCE_SUBDIR  ftviz_header_only)
FetchContent_MakeAvailable(xxhash glm)

add_library(ftviz_xxhash INTERFACE)
target_include_directories(ftviz_xxhash SYSTEM INTERFACE ${xxhash_SOURCE_DIR})
target_compile_definitions(ftviz_xxhash INTERFACE XXH_INLINE_ALL)

add_library(ftviz_glm INTERFACE)
target_include_directories(ftviz_glm SYSTEM INTERFACE ${glm_SOURCE_DIR})
target_compile_definitions(ftviz_glm INTERFACE GLM_FORCE_XYZW_ONLY GLM_ENABLE_EXPERIMENTAL)

if(FTVIZ_BUILD_APP)
  set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(GLFW_INSTALL        OFF CACHE BOOL "" FORCE)
  if(UNIX AND NOT APPLE)
    set(GLFW_BUILD_WAYLAND ${FTVIZ_GLFW_WAYLAND} CACHE BOOL "" FORCE)
    set(GLFW_BUILD_X11     ON                    CACHE BOOL "" FORCE)
  endif()
  FetchContent_Declare(glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG        3.4
    GIT_SHALLOW    TRUE)

  # glad 2.0.8: the generator runs at configure/build time (needs Python 3 with
  # jinja2). SOURCE_SUBDIR=cmake exposes glad_add_library().
  FetchContent_Declare(glad
    GIT_REPOSITORY https://github.com/Dav1dde/glad.git
    GIT_TAG        v2.0.8
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  cmake)
  FetchContent_MakeAvailable(glfw glad)
  # Core 4.5 plus the ARB/KHR extensions that make the documented GL 4.3
  # fallback work (DSA, immutable storage, clip control, robustness).
  set(FTVIZ_GL_EXTENSIONS
    GL_ARB_direct_state_access GL_ARB_buffer_storage GL_ARB_clip_control GL_KHR_robustness GL_KHR_debug)
  glad_add_library(ftviz_glad STATIC EXCLUDE_FROM_ALL REPRODUCIBLE
    API gl:core=4.5 EXTENSIONS ${FTVIZ_GL_EXTENSIONS})
  # Debug builds use glad's call wrappers instead. The Debug-only allocation
  # assertion hooks them to attribute allocations made inside GL calls (for
  # example a driver's LLVM shader JIT, whose C++ operator new resolves to our
  # counting replacement on ELF platforms) to the driver, not to our code.
  # DEBUG must precede API/EXTENSIONS (those keywords take lists).
  glad_add_library(ftviz_glad_debug STATIC EXCLUDE_FROM_ALL REPRODUCIBLE DEBUG
    API gl:core=4.5 EXTENSIONS ${FTVIZ_GL_EXTENSIONS})
endif()

# ----------------------------------------------------------------------------
# Warnings / optimisation helpers
# ----------------------------------------------------------------------------
include(CheckIPOSupported)
check_ipo_supported(RESULT FTVIZ_IPO_OK OUTPUT FTVIZ_IPO_MSG LANGUAGES CXX)

function(ftviz_target_defaults tgt)
  if(MSVC)
    target_compile_options(${tgt} PRIVATE /W4 /permissive- /Zc:__cplusplus /utf-8 /EHsc)
    target_compile_definitions(${tgt} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
    if(FTVIZ_WERROR)
      target_compile_options(${tgt} PRIVATE /WX)
    endif()
  else()
    target_compile_options(${tgt} PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wconversion
                                          -Wno-sign-conversion -Wnon-virtual-dtor)
    # errno is never read after math calls; this lets sqrt and friends
    # vectorise (results are unchanged: IEEE sqrt is still correctly rounded).
    target_compile_options(${tgt} PRIVATE -fno-math-errno)
    if(FTVIZ_WERROR)
      target_compile_options(${tgt} PRIVATE -Werror)
    endif()
    if(FTVIZ_NATIVE)
      target_compile_options(${tgt} PRIVATE -march=native)
    endif()
  endif()
  if(FTVIZ_IPO_OK)
    set_property(TARGET ${tgt} PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
    set_property(TARGET ${tgt} PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO TRUE)
  endif()
  target_include_directories(${tgt} PUBLIC ${PROJECT_SOURCE_DIR}/src)
endfunction()

# ----------------------------------------------------------------------------
# Libraries (one per mandatory module; render/app only with GL)
# ----------------------------------------------------------------------------
add_library(ftviz_core STATIC
  src/core/log.cpp
  src/core/json.cpp
  src/core/thread_pool.cpp
  src/core/file_util.cpp)
ftviz_target_defaults(ftviz_core)
target_link_libraries(ftviz_core PUBLIC Threads::Threads)

add_library(ftviz_ingest STATIC
  src/ingest/mapped_file.cpp
  src/ingest/chunker.cpp
  src/ingest/csv_parser.cpp
  src/ingest/field_parsers.cpp
  src/ingest/key_hash.cpp
  src/ingest/ftxb_format.cpp
  src/ingest/parse_worker.cpp
  src/ingest/entity_registry.cpp
  src/ingest/ingest.cpp)
ftviz_target_defaults(ftviz_ingest)
target_link_libraries(ftviz_ingest PUBLIC ftviz_core ftviz_xxhash)

add_library(ftviz_model STATIC
  src/model/edge_aggregation.cpp
  src/model/hub_taxonomy.cpp
  src/model/graph_builder.cpp
  src/model/corridors.cpp)
ftviz_target_defaults(ftviz_model)
target_link_libraries(ftviz_model PUBLIC ftviz_ingest ftviz_glm)

add_library(ftviz_layout STATIC
  src/layout/octree.cpp
  src/layout/initial_placement.cpp
  src/layout/force_solver.cpp
  src/layout/layout_engine.cpp)
ftviz_target_defaults(ftviz_layout)
target_link_libraries(ftviz_layout PUBLIC ftviz_model)

if(FTVIZ_BUILD_APP)
  add_library(ftviz_render STATIC
    src/render/gl_context.cpp
    src/render/shader_program.cpp
    src/render/camera.cpp
    src/render/font.cpp
    src/render/hud.cpp
    src/render/gpu_timers.cpp
    src/render/picking.cpp
    src/render/renderer.cpp)
  ftviz_target_defaults(ftviz_render)
  target_link_libraries(ftviz_render PUBLIC ftviz_layout $<IF:$<CONFIG:Debug>,ftviz_glad_debug,ftviz_glad> glfw)
  target_compile_definitions(ftviz_render PUBLIC GLFW_INCLUDE_NONE)

  add_executable(ftviz
    src/app/main.cpp
    src/app/cli.cpp
    src/app/render_config.cpp
    src/app/hud_stats.cpp
    src/app/application.cpp
    src/core/alloc_counter.cpp)
  ftviz_target_defaults(ftviz)
  target_link_libraries(ftviz PRIVATE ftviz_render)
  target_compile_definitions(ftviz PRIVATE
    FTVIZ_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
    $<$<CONFIG:Debug>:FTVIZ_COUNT_ALLOCATIONS=1>)
  # Ship shaders and config next to the binary so it runs from the build dir.
  add_custom_command(TARGET ftviz POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${PROJECT_SOURCE_DIR}/shaders $<TARGET_FILE_DIR:ftviz>/shaders
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${PROJECT_SOURCE_DIR}/config  $<TARGET_FILE_DIR:ftviz>/config)
endif()

if(FTVIZ_BUILD_TOOLS)
  add_executable(gen_synthetic tools/gen_synthetic.cpp)
  ftviz_target_defaults(gen_synthetic)
  target_link_libraries(gen_synthetic PRIVATE ftviz_ingest)
endif()

if(FTVIZ_BUILD_TESTS)
  enable_testing()
  add_executable(ftviz_tests
    tests/test_main.cpp
    tests/test_hash.cpp
    tests/test_csv.cpp
    tests/test_aggregation.cpp
    tests/test_octree.cpp
    tests/test_layout.cpp
    tests/test_core.cpp)
  ftviz_target_defaults(ftviz_tests)
  target_link_libraries(ftviz_tests PRIVATE ftviz_layout)
  target_compile_definitions(ftviz_tests PRIVATE FTVIZ_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
  add_test(NAME ftviz_tests COMMAND ftviz_tests)
endif()
