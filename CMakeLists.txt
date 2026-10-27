cmake_minimum_required(VERSION 3.15)
project(schwung-9w9 VERSION 0.1.0 LANGUAGES C)

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

set(W9_DSP_SOURCES
    src/dsp/9w9_plugin.c
    src/dsp/9w9_engine.c
)

# The Move module: dlopen'd by Schwung's chain host as dsp.so
add_library(9w9 SHARED ${W9_DSP_SOURCES})
target_include_directories(9w9 PRIVATE src src/dsp src/host)
target_link_libraries(9w9 PRIVATE m)
set_target_properties(9w9 PROPERTIES OUTPUT_NAME "dsp" PREFIX "" SUFFIX ".so")

# Offline renderer: same engine, writes WAVs so voices can be A/B'd
# against reference renders.
add_executable(9w9_render src/tools/render.c src/dsp/9w9_engine.c)
target_include_directories(9w9_render PRIVATE src src/dsp src/host)
target_link_libraries(9w9_render PRIVATE m)

# End-to-end loader test: dlopens the real dsp.so like the chain host does.
# Cross-compiled and run ON the Move.
add_executable(9w9_loadtest src/tools/loadtest.c)
target_include_directories(9w9_loadtest PRIVATE src src/dsp src/host)
target_link_libraries(9w9_loadtest PRIVATE dl m)
