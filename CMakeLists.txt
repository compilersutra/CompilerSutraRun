cmake_minimum_required(VERSION 3.20)
project(CompilerSutraRun
  VERSION 0.1.0
  DESCRIPTION "Universal GPU workload portability, execution, validation and benchmarking"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(CSRUN_ENABLE_VULKAN "Build Vulkan backend" ON)
option(CSRUN_ENABLE_OPENGL "Build OpenGL backend (EGL headless)" ON)
option(CSRUN_ENABLE_OPENCL "Build OpenCL backend" ON)
option(CSRUN_ENABLE_HIP "Build HIP/ROCm backend" ON)
option(CSRUN_ENABLE_CUDA "Build CUDA backend" OFF)
option(CSRUN_ENABLE_D3D12 "Build D3D12 backend (Windows)" OFF)
option(CSRUN_ENABLE_TESTS "Build unit tests" ON)
option(CSRUN_ENABLE_MLIR "Enable future MLIR integration hooks" OFF)

if(MSVC)
  add_compile_options(/W4)
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

FetchContent_Declare(
  cli11
  GIT_REPOSITORY https://github.com/CLIUtils/CLI11.git
  GIT_TAG v2.4.2
  GIT_SHALLOW TRUE)
FetchContent_Declare(
  json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.11.3
  GIT_SHALLOW TRUE)
FetchContent_Declare(
  yaml-cpp
  GIT_REPOSITORY https://github.com/jbeder/yaml-cpp.git
  GIT_TAG 0.8.0
  GIT_SHALLOW TRUE)

set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_INSTALL OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(cli11 json yaml-cpp)

if(CSRUN_ENABLE_TESTS)
  FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.14.0
    GIT_SHALLOW TRUE)
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
  enable_testing()
endif()

add_library(csrun_core
  src/core/Error.cpp
  src/core/Log.cpp
  src/core/Types.cpp
  src/core/Process.cpp)
target_include_directories(csrun_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(csrun_core PUBLIC cxx_std_20)

  add_library(csrun_compiler
  src/compiler/SpirvToolchain.cpp
  src/compiler/SpirvReflect.cpp)
target_link_libraries(csrun_compiler PUBLIC csrun_core)
target_include_directories(csrun_compiler PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_library(csrun_result
  src/result/RunResult.cpp
  src/result/JsonWriter.cpp)
target_link_libraries(csrun_result PUBLIC csrun_core csrun_benchmark nlohmann_json::nlohmann_json)
target_include_directories(csrun_result PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_library(csrun_benchmark
  src/benchmark/Statistics.cpp
  src/benchmark/Timing.cpp
  src/benchmark/PipelineStats.cpp
  src/benchmark/PowerSampler.cpp)
target_link_libraries(csrun_benchmark PUBLIC csrun_core)
target_include_directories(csrun_benchmark PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_library(csrun_validation
  src/validation/Comparator.cpp
  src/validation/FrameCheck.cpp)
target_link_libraries(csrun_validation PUBLIC csrun_core)
target_include_directories(csrun_validation PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_library(csrun_workload
  src/workload/Workload.cpp
  src/workload/WorkloadParser.cpp
  src/workload/VectorAddScaffold.cpp
  src/workload/MatmulScaffold.cpp)
target_link_libraries(csrun_workload PUBLIC
  csrun_core
  csrun_validation
  csrun_benchmark
  csrun_result
  yaml-cpp)
target_include_directories(csrun_workload PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_library(csrun_device
  src/device/DeviceInfo.cpp)
target_link_libraries(csrun_device PUBLIC csrun_core)
target_include_directories(csrun_device PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_library(csrun_backend
  src/backend/BackendType.cpp
  src/backend/BackendRegistry.cpp
  src/backend/BackendSelector.cpp)
target_link_libraries(csrun_backend PUBLIC csrun_core csrun_device csrun_workload csrun_result)
target_include_directories(csrun_backend PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

set(CSRUN_HAS_VULKAN_VALUE 0)
set(CSRUN_HAS_OPENGL_VALUE 0)
set(CSRUN_HAS_OPENCL_VALUE 0)
set(CSRUN_HAS_HIP_VALUE 0)

if(CSRUN_ENABLE_VULKAN)
  find_package(Vulkan QUIET)
  if(Vulkan_FOUND)
    add_library(csrun_backend_vulkan
      src/backends/vulkan/VulkanBackend.cpp
      src/backends/vulkan/VulkanSpirvRunner.cpp
      src/backends/vulkan/VulkanRayTracingSmoke.cpp
      src/backends/vulkan/VulkanGraphicsResources.cpp
      src/backends/vulkan/VulkanComputeResources.cpp
      src/backends/vulkan/VulkanWsiPresenter.cpp
      src/backends/vulkan/VulkanPerfQuery.cpp)
    target_link_libraries(csrun_backend_vulkan PUBLIC
      csrun_compiler
      csrun_workload
      csrun_validation
      csrun_benchmark
      csrun_result
      Vulkan::Vulkan)
    find_package(X11 REQUIRED)
    target_include_directories(csrun_backend_vulkan PRIVATE ${X11_INCLUDE_DIR})
    target_link_libraries(csrun_backend_vulkan PUBLIC ${X11_LIBRARIES})
    message(STATUS "CSRUN: Vulkan WSI present ENABLED (VK_KHR_xlib_surface + swapchain)")
    target_include_directories(csrun_backend_vulkan PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_compile_definitions(csrun_backend_vulkan PUBLIC CSRUN_HAS_VULKAN=1)
    set(CSRUN_HAS_VULKAN_VALUE 1)
    message(STATUS "CSRUN: Vulkan backend ENABLED")
  else()
    message(WARNING "CSRUN_ENABLE_VULKAN=ON but Vulkan not found; building without Vulkan")
  endif()
endif()

if(CSRUN_ENABLE_OPENGL)
  find_package(OpenGL QUIET)
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(EGL QUIET egl)
  endif()
  if(OpenGL_FOUND AND EGL_FOUND)
    add_library(csrun_backend_opengl src/backends/opengl/OpenGLBackend.cpp)
    target_include_directories(csrun_backend_opengl PUBLIC
      ${CMAKE_CURRENT_SOURCE_DIR}/include
      ${EGL_INCLUDE_DIRS})
    target_link_libraries(csrun_backend_opengl PUBLIC
      csrun_workload
      csrun_validation
      csrun_benchmark
      csrun_result
      OpenGL::GL
      ${EGL_LIBRARIES})
    target_compile_definitions(csrun_backend_opengl PUBLIC CSRUN_HAS_OPENGL=1)
    set(CSRUN_HAS_OPENGL_VALUE 1)
    message(STATUS "CSRUN: OpenGL backend ENABLED")
  else()
    message(WARNING "CSRUN_ENABLE_OPENGL=ON but OpenGL/EGL not found; building without OpenGL")
  endif()
endif()

if(CSRUN_ENABLE_OPENCL)
  find_path(OpenCL_INCLUDE_DIR CL/cl.h PATHS /usr/include /usr/local/include)
  find_library(OpenCL_LIBRARY NAMES OpenCL OpenCL.so.1 libOpenCL.so.1
    PATHS /usr/lib /usr/lib64 /usr/lib/x86_64-linux-gnu /usr/local/lib
    NO_DEFAULT_PATH)
  if(NOT OpenCL_LIBRARY)
    find_library(OpenCL_LIBRARY NAMES OpenCL OpenCL.so.1 libOpenCL.so.1)
  endif()
  if(NOT OpenCL_LIBRARY AND EXISTS /usr/lib/x86_64-linux-gnu/libOpenCL.so.1)
    set(OpenCL_LIBRARY /usr/lib/x86_64-linux-gnu/libOpenCL.so.1)
  endif()
  if(OpenCL_INCLUDE_DIR AND OpenCL_LIBRARY)
    add_library(csrun_backend_opencl src/backends/opencl/OpenCLBackend.cpp)
    target_include_directories(csrun_backend_opencl PUBLIC
      ${CMAKE_CURRENT_SOURCE_DIR}/include
      ${OpenCL_INCLUDE_DIR})
    target_link_libraries(csrun_backend_opencl PUBLIC
      csrun_workload
      csrun_validation
      csrun_benchmark
      csrun_result
      ${OpenCL_LIBRARY})
    target_compile_definitions(csrun_backend_opencl PUBLIC CSRUN_HAS_OPENCL=1)
    set(CSRUN_HAS_OPENCL_VALUE 1)
    message(STATUS "CSRUN: OpenCL backend ENABLED (${OpenCL_LIBRARY})")
  else()
    message(WARNING "CSRUN_ENABLE_OPENCL=ON but OpenCL not found")
  endif()
endif()

if(CSRUN_ENABLE_HIP)
  find_path(HIP_INCLUDE_DIR hip/hip_runtime.h
    PATHS /opt/rocm/include /opt/rocm-7.0.2/include ENV ROCM_PATH
    PATH_SUFFIXES include)
  find_library(HIP_LIBRARY NAMES amdhip64
    PATHS /opt/rocm/lib /opt/rocm-7.0.2/lib ENV ROCM_PATH PATH_SUFFIXES lib)
  find_library(HIPRTC_LIBRARY NAMES hiprtc
    PATHS /opt/rocm/lib /opt/rocm-7.0.2/lib ENV ROCM_PATH PATH_SUFFIXES lib)
  if(HIP_INCLUDE_DIR AND HIP_LIBRARY AND HIPRTC_LIBRARY)
    add_library(csrun_backend_hip src/backends/hip/HipBackend.cpp)
    target_include_directories(csrun_backend_hip PUBLIC
      ${CMAKE_CURRENT_SOURCE_DIR}/include
      ${HIP_INCLUDE_DIR})
    target_link_libraries(csrun_backend_hip PUBLIC
      csrun_workload
      csrun_validation
      csrun_benchmark
      csrun_result
      ${HIP_LIBRARY}
      ${HIPRTC_LIBRARY})
    target_compile_definitions(csrun_backend_hip PUBLIC CSRUN_HAS_HIP=1 __HIP_PLATFORM_AMD__)
    set_target_properties(csrun_backend_hip PROPERTIES
      BUILD_RPATH "/opt/rocm/lib"
      INSTALL_RPATH "/opt/rocm/lib")
    set(CSRUN_HAS_HIP_VALUE 1)
    message(STATUS "CSRUN: HIP/ROCm backend ENABLED (${HIP_LIBRARY})")
  else()
    message(WARNING "CSRUN_ENABLE_HIP=ON but HIP/hiprtc not found")
  endif()
endif()

add_library(csrun_runtime
  src/runtime/PlatformRuntime.cpp
  src/runtime/BuiltinBackends.cpp)
target_link_libraries(csrun_runtime PUBLIC
  csrun_core
  csrun_compiler
  csrun_workload
  csrun_backend
  csrun_device
  csrun_validation
  csrun_benchmark
  csrun_result)
if(CSRUN_HAS_VULKAN_VALUE)
  target_link_libraries(csrun_runtime PUBLIC csrun_backend_vulkan)
  target_compile_definitions(csrun_runtime PRIVATE CSRUN_HAS_VULKAN=1)
endif()
if(CSRUN_HAS_OPENGL_VALUE)
  target_link_libraries(csrun_runtime PUBLIC csrun_backend_opengl)
  target_compile_definitions(csrun_runtime PRIVATE CSRUN_HAS_OPENGL=1)
endif()
if(CSRUN_HAS_OPENCL_VALUE)
  target_link_libraries(csrun_runtime PUBLIC csrun_backend_opencl)
  target_compile_definitions(csrun_runtime PRIVATE CSRUN_HAS_OPENCL=1)
endif()
if(CSRUN_HAS_HIP_VALUE)
  target_link_libraries(csrun_runtime PUBLIC csrun_backend_hip)
  target_compile_definitions(csrun_runtime PRIVATE CSRUN_HAS_HIP=1)
endif()
target_include_directories(csrun_runtime PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_executable(csrun tools/csrun/main.cpp)
target_link_libraries(csrun PRIVATE csrun_runtime CLI11::CLI11)
set_target_properties(csrun PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
  BUILD_RPATH "/opt/rocm/lib"
  INSTALL_RPATH "/opt/rocm/lib")
if(CSRUN_HAS_VULKAN_VALUE)
  target_compile_definitions(csrun PRIVATE CSRUN_HAS_VULKAN=1)
endif()

if(CSRUN_ENABLE_CUDA)
  message(STATUS "CSRUN: CUDA backend requested (needs NVIDIA GPU + CUDA toolkit)")
endif()
if(CSRUN_ENABLE_D3D12)
  message(STATUS "CSRUN: D3D12 backend requested (Windows)")
endif()

if(CSRUN_ENABLE_TESTS)
  add_executable(csrun_unit_tests
    tests/unit/core/ErrorTest.cpp
    tests/unit/workload/WorkloadParserTest.cpp
    tests/unit/workload/VectorAddScaffoldTest.cpp
    tests/unit/backend/BackendSelectorTest.cpp
    tests/unit/benchmark/StatisticsTest.cpp
    tests/unit/validation/ComparatorTest.cpp
    tests/unit/validation/FrameCheckTest.cpp
    tests/unit/result/JsonWriterTest.cpp
    tests/unit/compiler/SpirvReflectTest.cpp
    tests/unit/backends/vulkan/VulkanHandleTest.cpp)
  target_link_libraries(csrun_unit_tests PRIVATE csrun_runtime GTest::gtest_main)
  include(GoogleTest)
  gtest_discover_tests(csrun_unit_tests)

  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_Interpreter_FOUND)
    add_custom_target(check-csrun
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/csrun-lit"
              "${CMAKE_SOURCE_DIR}/tests/lit"
              --csrun "$<TARGET_FILE:csrun>"
              --repo "${CMAKE_SOURCE_DIR}"
      DEPENDS csrun
      WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
      COMMENT "Running csrun-lit regression suite"
      USES_TERMINAL)
  endif()
endif()

message(STATUS "CompilerSutraRun ${PROJECT_VERSION}")
message(STATUS "  CSRUN_ENABLE_VULKAN=${CSRUN_ENABLE_VULKAN} (active=${CSRUN_HAS_VULKAN_VALUE})")
message(STATUS "  CSRUN_ENABLE_OPENGL=${CSRUN_ENABLE_OPENGL} (active=${CSRUN_HAS_OPENGL_VALUE})")
message(STATUS "  CSRUN_ENABLE_OPENCL=${CSRUN_ENABLE_OPENCL} (active=${CSRUN_HAS_OPENCL_VALUE})")
message(STATUS "  CSRUN_ENABLE_HIP=${CSRUN_ENABLE_HIP} (active=${CSRUN_HAS_HIP_VALUE})")
message(STATUS "  CSRUN_ENABLE_CUDA=${CSRUN_ENABLE_CUDA}")
message(STATUS "  CSRUN_ENABLE_D3D12=${CSRUN_ENABLE_D3D12}")
message(STATUS "  CSRUN_ENABLE_TESTS=${CSRUN_ENABLE_TESTS}")
