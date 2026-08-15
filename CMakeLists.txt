cmake_minimum_required(VERSION 3.15)
project(schwung-er99 VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
# No -ffast-math: it lets the compiler assume no NaN/Inf, which removes the
# safety of our clamps on the realtime path. The DSP is nowhere near hot
# enough on this SoC to need it.
set(CMAKE_C_FLAGS_RELEASE "-O3")

set(ER99_DSP_SOURCES
    src/dsp/er99_plugin.c
    src/dsp/er99_engine.c
)

# The Move module: dlopen'd by Schwung's chain host as dsp.so
add_library(er99 SHARED ${ER99_DSP_SOURCES})
target_include_directories(er99 PRIVATE src src/dsp src/host)
target_link_libraries(er99 PRIVATE m)
set_target_properties(er99 PROPERTIES OUTPUT_NAME "dsp" PREFIX "" SUFFIX ".so")

# Offline renderer: same engine, writes WAVs so the C port can be A/B'd
# against the Node/Web Audio reference renders.
add_executable(er99_render src/tools/render.c src/dsp/er99_engine.c)
target_include_directories(er99_render PRIVATE src src/dsp src/host)
target_link_libraries(er99_render PRIVATE m)

# End-to-end loader test: dlopens the real dsp.so like the chain host does.
# Cross-compiled and run ON the Move.
add_executable(er99_loadtest src/tools/loadtest.c)
target_include_directories(er99_loadtest PRIVATE src src/dsp src/host)
target_link_libraries(er99_loadtest PRIVATE dl m)
