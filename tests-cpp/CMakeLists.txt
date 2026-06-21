cmake_minimum_required(VERSION 3.20)

# ============================================================================
# Out-of-game unit tests for the engine-free plugin logic (vocabulary aliases +
# command grammar). Compiles the same source files the plugin uses — no RE::,
# no game, no recognizer. Run: ctest --test-dir build  (or run the exe directly).
# ============================================================================
project(VoiceSpellcastingTests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

enable_testing()

add_executable(grammar_tests
    test_grammar.cpp
    ../src/Vocabulary.cpp
    ../src/CommandGrammar.cpp
)
target_include_directories(grammar_tests PRIVATE ../src)
if(MSVC)
    target_compile_options(grammar_tests PRIVATE /EHsc /utf-8 /W4)
endif()

add_test(NAME grammar_tests COMMAND grammar_tests)
