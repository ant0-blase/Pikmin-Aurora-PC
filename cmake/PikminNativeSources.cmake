file(GLOB_RECURSE PIKMIN_NATIVE_CPP CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/*.cpp")
file(GLOB_RECURSE PIKMIN_NATIVE_C CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/*.c")
set(PIKMIN_NATIVE_SOURCES ${PIKMIN_NATIVE_CPP} ${PIKMIN_NATIVE_C})

# The native JAudio/smssynth bridge is compiled by its dedicated C++23
# object target in the top-level CMakeLists.txt. Keep it out of the regular
# Pikmin source glob, which otherwise compiles it a second time as C++20 and
# without the phosg/resource_dasm include paths.
list(FILTER PIKMIN_NATIVE_SOURCES EXCLUDE REGEX "/src/port/jaudio_smssynth_bridge\\.cpp$")
list(FILTER PIKMIN_NATIVE_SOURCES EXCLUDE REGEX "/src/port/jaudio_smssynth_bridge_stub\\.cpp$")

# Aurora replaces the low-level GameCube SDK implementations.
set(_sdk_dirs
  MSL_C OdemuExi2 Runtime TRK_MINNOW_DOLPHIN amcExi2 amcnotstub amcstubs
  ar base card db dsp dvd exi gx mtx mtxD odemustubs odenotstub os pad si vi
)
foreach(_dir IN LISTS _sdk_dirs)
  list(FILTER PIKMIN_NATIVE_SOURCES EXCLUDE REGEX "/src/${_dir}/")
endforeach()

# Windows-only developer/debug UI and the legacy OpenGL renderer.
list(FILTER PIKMIN_NATIVE_SOURCES EXCLUDE REGEX "/src/sysCore/(attachModule|atxDirectRouter|fileIO|moduleMgr|oglGraphics|oglShapeOpt|tcpStream|uiCore|uiRenderWindow|uiTools|uiWindow|wSocket)\\.cpp$")

# The original GameCube DSP/JAudio implementation is not host-safe. The native
# port supplies its own SDL3/STX path and optional software JAudio bridge.
# src/ai directly touches GameCube DSP/MMIO registers, so it stays out too.
list(FILTER PIKMIN_NATIVE_SOURCES EXCLUDE REGEX "/src/(ai|jaudio|hvqm4dec)/")
# HIO is only a development/debug transport on PC; host shim provides inert entry points.
list(FILTER PIKMIN_NATIVE_SOURCES EXCLUDE REGEX "/src/hio/")

# Aurora provides the process main(); Pikmin exposes pikmin_game_main() from sysBootup.cpp.
list(APPEND PIKMIN_NATIVE_SOURCES
  "${CMAKE_SOURCE_DIR}/src/port/aurora_main.cpp"
  "${CMAKE_SOURCE_DIR}/src/port/host_os.cpp"
  "${CMAKE_SOURCE_DIR}/src/port/audio_stubs.cpp"
  # The retail BARC/sequence header is embedded in this decomp source.  The
  # native audio bridge writes it to the host cache beside pikiseq.arc so the
  # software JAudio renderer can resolve sequence names without an external
  # extraction step.
  "${CMAKE_SOURCE_DIR}/src/jaudio/pikiseq.c"
)

# Several original .c files contain Metrowerks C++ extensions / C++ headers.
set_source_files_properties(${PIKMIN_NATIVE_C} PROPERTIES LANGUAGE CXX)
