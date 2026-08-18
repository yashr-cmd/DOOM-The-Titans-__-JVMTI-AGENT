cmake_minimum_required(VERSION 3.10)
project(dtt_native_agent C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# --------------------------------------------------------------------------
# JAVA_HOME must be set (or -DJAVA_HOME=... passed) so we can find jni.h,
# jvmti.h, and the platform-specific jni_md.h. Any JDK 8+ works; JVMTI's
# ABI has been stable for this feature set for a long time.
# --------------------------------------------------------------------------
if(NOT DEFINED JAVA_HOME)
    if(DEFINED ENV{JAVA_HOME})
        set(JAVA_HOME $ENV{JAVA_HOME})
    else()
        message(FATAL_ERROR "JAVA_HOME is not set. Pass -DJAVA_HOME=/path/to/jdk or set the JAVA_HOME environment variable.")
    endif()
endif()

include_directories(${JAVA_HOME}/include)

if(WIN32)
    include_directories(${JAVA_HOME}/include/win32)
elseif(APPLE)
    include_directories(${JAVA_HOME}/include/darwin)
elseif(UNIX)
    include_directories(${JAVA_HOME}/include/linux)
endif()

include_directories(${CMAKE_SOURCE_DIR}/include)

file(GLOB DTT_SOURCES ${CMAKE_SOURCE_DIR}/src/*.c)

add_library(dtt_agent SHARED ${DTT_SOURCES})

# Output name convention per platform:
#   Windows -> dtt_agent.dll
#   Linux   -> libdtt_agent.so
#   macOS   -> libdtt_agent.dylib
set_target_properties(dtt_agent PROPERTIES
    OUTPUT_NAME "dtt_agent"
    PREFIX "${CMAKE_SHARED_LIBRARY_PREFIX}"
)

if(WIN32)
    # Pure C, no MSVC C++ runtime dependency needed.
    target_compile_definitions(dtt_agent PRIVATE _CRT_SECURE_NO_WARNINGS)
elseif(APPLE)
    target_link_libraries(dtt_agent pthread)
elseif(UNIX)
    target_link_libraries(dtt_agent pthread)
endif()

# Warnings-as-diagnostics (not errors) to catch mistakes early without
# turning an unrelated toolchain warning into a hard build failure.
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(dtt_agent PRIVATE -Wall -Wextra)
elseif(MSVC)
    target_compile_options(dtt_agent PRIVATE /W4)
endif()
