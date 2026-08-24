cmake_minimum_required(VERSION 3.25)
project(PikminAurora LANGUAGES C CXX)

if(NOT UNIX OR APPLE)
  message(FATAL_ERROR "This native target currently supports Linux only.")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(PIKMIN_VERSION "GPIE01_01" CACHE STRING "Pikmin disc revision")
option(PIKMIN_NATIVE_CPU "Enable -march/-mtune=native" ON)
option(PIKMIN_LTO "Enable link-time optimisation" ON)
option(PIKMIN_FAST_MATH "Enable unsafe fast-math optimisations" OFF)
option(PIKMIN_NATIVE_JAUDIO "Enable experimental software JAudio synthesis" ON)

if(NOT EXISTS "${CMAKE_SOURCE_DIR}/extern/aurora/CMakeLists.txt")
  message(FATAL_ERROR "Aurora is missing. Run ./build.sh; it bootstraps extern/aurora automatically.")
endif()

# The optional native JAudio bridge embeds resource_dasm's BMS renderer in a
# dedicated C++23 translation unit. build.sh pins/builds the dependencies.
set(PIKMIN_AUDIO_PREFIX "${CMAKE_SOURCE_DIR}/extern/audio-prefix")
set(PIKMIN_RESOURCE_DASM_DIR "${CMAKE_SOURCE_DIR}/extern/resource_dasm")
set(PIKMIN_RESOURCE_FILE_LIB "${CMAKE_SOURCE_DIR}/build/audio-deps/resource_dasm/libresource_file.a")
set(PIKMIN_PHOSG_LIB "${PIKMIN_AUDIO_PREFIX}/lib/libphosg.a")
if(PIKMIN_NATIVE_JAUDIO)
  if(NOT EXISTS "${PIKMIN_RESOURCE_DASM_DIR}/src/Audio/smssynth.cc" OR
     NOT EXISTS "${PIKMIN_RESOURCE_FILE_LIB}" OR
     NOT EXISTS "${PIKMIN_PHOSG_LIB}")
    message(FATAL_ERROR "Native JAudio dependencies are missing. Run ./build.sh; it bootstraps pinned phosg/resource_dasm automatically.")
  endif()
endif()

# Only build the Aurora subsystems needed by Pikmin.
set(AURORA_ENABLE_GX ON CACHE BOOL "" FORCE)
set(AURORA_ENABLE_DVD ON CACHE BOOL "" FORCE)
set(AURORA_ENABLE_CARD ON CACHE BOOL "" FORCE)
set(AURORA_ENABLE_RMLUI OFF CACHE BOOL "" FORCE)
set(AURORA_CACHE_USE_ZSTD ON CACHE BOOL "" FORCE)

# Arch Linux can ship an Abseil CMake package whose headers and shared
# libraries are temporarily out of sync during rolling updates. Aurora links
# Abseil into several static libraries, so pin the same standalone revision
# Aurora itself falls back to and create the absl:: targets before Aurora is
# configured. This keeps the native port independent from /usr/lib/libabsl_*.
include(FetchContent)
set(ABSL_PROPAGATE_CXX_STD ON CACHE BOOL "" FORCE)
set(ABSL_BUILD_TESTING OFF CACHE BOOL "" FORCE)
FetchContent_Declare(abseil-cpp
  URL https://github.com/abseil/abseil-cpp/archive/refs/tags/20240722.0.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP FALSE
  EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(abseil-cpp)

if(NOT TARGET absl::flat_hash_map OR NOT TARGET absl::btree)
  message(FATAL_ERROR "Bundled Abseil did not provide the targets required by Aurora")
endif()

add_subdirectory(extern/aurora EXCLUDE_FROM_ALL)

include(cmake/PikminNativeSources.cmake)
add_executable(pikmin ${PIKMIN_NATIVE_SOURCES})

if(PIKMIN_NATIVE_JAUDIO)
  add_library(pikmin_jaudio_bridge OBJECT src/port/jaudio_smssynth_bridge.cpp)
  target_compile_features(pikmin_jaudio_bridge PRIVATE cxx_std_23)
  target_include_directories(pikmin_jaudio_bridge PRIVATE
    "${PIKMIN_RESOURCE_DASM_DIR}/src"
    "${PIKMIN_AUDIO_PREFIX}/include"
  )
  target_compile_options(pikmin_jaudio_bridge PRIVATE
    $<$<CONFIG:Release>:-O3>
    $<$<CONFIG:RelWithDebInfo>:-O3>
    -fno-plt
    -fno-omit-frame-pointer
  )
  if(PIKMIN_NATIVE_CPU)
    target_compile_options(pikmin_jaudio_bridge PRIVATE -march=native -mtune=native)
  endif()
  target_sources(pikmin PRIVATE $<TARGET_OBJECTS:pikmin_jaudio_bridge>)
else()
  target_sources(pikmin PRIVATE src/port/jaudio_smssynth_bridge_stub.cpp)
endif()

target_include_directories(pikmin PRIVATE "${CMAKE_SOURCE_DIR}/include")
target_compile_definitions(pikmin PRIVATE
  TARGET_PC=1
  DTK_CONFIG_NONMATCHING=1
  PIKI_USE_JAUDIO=0
  "VERSION_${PIKMIN_VERSION}=1"
)

target_compile_options(pikmin PRIVATE
  $<$<COMPILE_LANGUAGE:CXX>:-fpermissive>
  $<$<CONFIG:Release>:-O3>
  $<$<CONFIG:RelWithDebInfo>:-O3>
  -fno-plt
  -fno-omit-frame-pointer
)
if(PIKMIN_NATIVE_CPU)
  target_compile_options(pikmin PRIVATE -march=native -mtune=native)
endif()
if(PIKMIN_FAST_MATH)
  target_compile_options(pikmin PRIVATE -ffast-math)
endif()
if(PIKMIN_LTO)
  include(CheckIPOSupported)
  check_ipo_supported(RESULT _ipo_ok OUTPUT _ipo_error)
  if(_ipo_ok)
    set_property(TARGET pikmin PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
    set_property(TARGET pikmin PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO TRUE)
  else()
    message(WARNING "IPO/LTO unavailable: ${_ipo_error}")
  endif()
endif()

# Keep dead-code elimination enabled.  -rdynamic forces otherwise-unused Aurora
# archive members to stay live under LTO and pulls in optional Abseil symbols.
target_link_libraries(pikmin PRIVATE
  z
  aurora::core
  aurora::os
  aurora::gx
  aurora::gd
  aurora::pad
  aurora::si
  aurora::main
  aurora::mtx
  aurora::vi
  aurora::card
  aurora::dvd
  ${AURORA_SDL3_TARGET}
  pthread
  m
)
if(PIKMIN_NATIVE_JAUDIO)
  target_link_libraries(pikmin PRIVATE
    "${PIKMIN_RESOURCE_FILE_LIB}"
    "${PIKMIN_PHOSG_LIB}"
  )
endif()

set_target_properties(pikmin PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
