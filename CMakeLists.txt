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

add_library(csrun_workload
  src/workload/Workload.cpp
  src/workload/WorkloadParser.cpp)
target_link_libraries(csrun_workload PUBLIC csrun_core yaml-cpp)
target_include_directories(csrun_workload PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_library(csrun_device
  src/device/DeviceInfo.cpp)
target_link_libraries(csrun_device PUBLIC csrun_core)
target_include_directories(csrun_device PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_library(csrun_backend
  src/backend/BackendType.cpp
  src/backend/BackendRegistry.cpp
  src/backend/BackendSelector.cpp)
target_link_libraries(csrun_backend PUBLIC csrun_core csrun_device csrun_workload)
target_include_directories(csrun_backend PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_library(csrun_validation
  src/validation/Comparator.cpp)
target_link_libraries(csrun_validation PUBLIC csrun_core)
target_include_directories(csrun_validation PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_library(csrun_benchmark
  src/benchmark/Statistics.cpp)
target_link_libraries(csrun_benchmark PUBLIC csrun_core)
target_include_directories(csrun_benchmark PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_library(csrun_result
  src/result/RunResult.cpp
  src/result/JsonWriter.cpp)
target_link_libraries(csrun_result PUBLIC csrun_core nlohmann_json::nlohmann_json)
target_include_directories(csrun_result PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

set(CSRUN_HAS_VULKAN_VALUE 0)
if(CSRUN_ENABLE_VULKAN)
  find_package(Vulkan QUIET)
  if(Vulkan_FOUND)
    add_library(csrun_backend_vulkan
      src/backends/vulkan/VulkanBackend.cpp)
    target_link_libraries(csrun_backend_vulkan PUBLIC
      csrun_core
      csrun_workload
      csrun_validation
      csrun_benchmark
      csrun_result
      Vulkan::Vulkan)
    target_include_directories(csrun_backend_vulkan PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_compile_definitions(csrun_backend_vulkan PUBLIC CSRUN_HAS_VULKAN=1)
    set(CSRUN_HAS_VULKAN_VALUE 1)
    message(STATUS "CSRUN: Vulkan backend ENABLED")
  else()
    message(WARNING "CSRUN_ENABLE_VULKAN=ON but Vulkan not found; building without Vulkan")
  endif()
endif()

add_library(csrun_runtime
  src/runtime/PlatformRuntime.cpp)
target_link_libraries(csrun_runtime PUBLIC
  csrun_core
  csrun_workload
  csrun_backend
  csrun_device
  csrun_validation
  csrun_benchmark
  csrun_result)
if(CSRUN_HAS_VULKAN_VALUE)
  target_link_libraries(csrun_runtime PUBLIC csrun_backend_vulkan)
  target_compile_definitions(csrun_runtime PUBLIC CSRUN_HAS_VULKAN=1)
  target_compile_definitions(csrun_backend PUBLIC CSRUN_HAS_VULKAN=1)
endif()
target_include_directories(csrun_runtime PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_executable(csrun tools/csrun/main.cpp)
target_link_libraries(csrun PRIVATE csrun_runtime CLI11::CLI11)
set_target_properties(csrun PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

if(CSRUN_ENABLE_CUDA)
  message(STATUS "CSRUN: CUDA backend requested (implementation lands in Phase 4)")
endif()
if(CSRUN_ENABLE_D3D12)
  message(STATUS "CSRUN: D3D12 backend requested (Windows / Phase 7)")
endif()

if(CSRUN_ENABLE_TESTS)
  add_executable(csrun_unit_tests
    tests/unit/core/ErrorTest.cpp
    tests/unit/workload/WorkloadParserTest.cpp
    tests/unit/backend/BackendSelectorTest.cpp
    tests/unit/benchmark/StatisticsTest.cpp
    tests/unit/validation/ComparatorTest.cpp
    tests/unit/result/JsonWriterTest.cpp)
  target_link_libraries(csrun_unit_tests PRIVATE
    csrun_runtime
    GTest::gtest_main)
  include(GoogleTest)
  gtest_discover_tests(csrun_unit_tests)
endif()

message(STATUS "CompilerSutraRun ${PROJECT_VERSION}")
message(STATUS "  CSRUN_ENABLE_VULKAN=${CSRUN_ENABLE_VULKAN} (active=${CSRUN_HAS_VULKAN_VALUE})")
message(STATUS "  CSRUN_ENABLE_CUDA=${CSRUN_ENABLE_CUDA}")
message(STATUS "  CSRUN_ENABLE_D3D12=${CSRUN_ENABLE_D3D12}")
message(STATUS "  CSRUN_ENABLE_TESTS=${CSRUN_ENABLE_TESTS}")
