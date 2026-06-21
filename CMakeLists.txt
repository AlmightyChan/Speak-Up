cmake_minimum_required(VERSION 3.24)

# ============================================================================
# Speak Up — plug-and-play, Win11-proof offline voice spellcasting for
# Skyrim SE/AE. Reads the player's live spellbook and recognizes spoken
# commands in-process (no companion process, no IPC) via Vosk or sherpa-onnx
# loaded as DLLs at runtime. Built on CommonLibSSE-NG (GPL-3.0).
# Spiritual successor to Dragonborn Unlimited / Dragonborn Speaks Naturally;
# no code from either is used.
# ============================================================================
project(
    VoiceSpellcasting
    VERSION 1.2.1
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_DEBUG OFF)

if(MSVC)
    add_compile_options(
        /MP /W4 /WX /permissive- /Zc:preprocessor /EHsc /utf-8
        /wd4068 /wd4189 /wd4200 /wd4201 /wd4324
    )
endif()

# ----------------------------------------------------------------------------
# Dependencies — provided by vcpkg (see vcpkg.json + vcpkg-configuration.json).
# CommonLibSSE-NG comes from the colorglass registry; statically linked.
# Configure with the vcpkg toolchain + triplet x64-windows-static-md (see
# CMakePresets.json).
# ----------------------------------------------------------------------------
find_package(CommonLibSSE REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

# User-facing plugin name (DLL + log + SKSE version data). The internal CMake
# project/target stays VoiceSpellcasting; only the OUTPUT_NAME differs.
set(PLUGIN_NAME "SpeakUp")
set(PLUGIN_AUTHOR "Ethan Muna")
set(PLUGIN_VERSION "${PROJECT_VERSION}")

# ----------------------------------------------------------------------------
# Sources — grows incrementally per the phased plan. Phase 0: load + log only.
# Only list files that exist.
# ----------------------------------------------------------------------------
set(SOURCES
    src/main.cpp
    src/SpellRoster.cpp
    src/Equipper.cpp
    src/TestHarness.cpp
    src/Recognizer.cpp
    src/SherpaRecognizer.cpp
    src/VoiceController.cpp
    src/Vocabulary.cpp
    src/CommandGrammar.cpp
    src/GlobalCommands.cpp
)
set(HEADERS
    src/PCH.h
    src/Types.h
    src/SpellRoster.h
    src/Equipper.h
    src/TestHarness.h
    src/Recognizer.h
    src/VoskLoader.h
    src/SherpaLoader.h
    src/SherpaRecognizer.h
    src/MicCapture.h
    src/VoiceController.h
    src/PhraseNormalize.h
    src/Vocabulary.h
    src/CommandGrammar.h
    src/FuzzyMatch.h
)

add_library(${PROJECT_NAME} SHARED ${SOURCES} ${HEADERS})

target_compile_definitions(${PROJECT_NAME} PRIVATE
    PLUGIN_NAME="${PLUGIN_NAME}"
    PLUGIN_AUTHOR="${PLUGIN_AUTHOR}"
    PLUGIN_VERSION="${PLUGIN_VERSION}"
    PLUGIN_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    PLUGIN_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    PLUGIN_VERSION_PATCH=${PROJECT_VERSION_PATCH}
)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/extern
)
target_link_libraries(${PROJECT_NAME} PRIVATE CommonLibSSE::CommonLibSSE nlohmann_json::nlohmann_json winmm)
target_precompile_headers(${PROJECT_NAME} PRIVATE src/PCH.h)

set_target_properties(${PROJECT_NAME} PROPERTIES
    OUTPUT_NAME "${PLUGIN_NAME}"
    SUFFIX ".dll"
)

# Auto-deploy the built DLL to the MO2 mod folder (set VSC_DEPLOY_DIR in the preset).
# The destination (may contain spaces) is baked into a configured script so it never
# rides on a command line. A locked destination warns instead of failing the build.
if(DEFINED VSC_DEPLOY_DIR)
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/deploy.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/deploy_configured.cmake"
        @ONLY)
    add_custom_command(
        TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND "${CMAKE_COMMAND}"
                -DSRC=$<TARGET_FILE:${PROJECT_NAME}>
                -P "${CMAKE_CURRENT_BINARY_DIR}/deploy_configured.cmake"
        VERBATIM)
endif()
