cmake_minimum_required(VERSION 3.22)

project(GrainLock VERSION 0.2.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(GRAINLOCK_BUILD_PLUGIN "Build the GrainLock VST3 plugin" ON)
option(GRAINLOCK_BUILD_TESTS  "Build the offline GrainLock test runner" ON)

# Static MSVC runtime: the plugin then needs no Visual C++ redistributable.
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# JUCE is pinned to one JUCE 8 release so every build uses identical framework code.
include(FetchContent)
set(FETCHCONTENT_QUIET OFF)
FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        8.0.15
    GIT_SHALLOW    TRUE
    GIT_PROGRESS   TRUE)
FetchContent_MakeAvailable(JUCE)

# Sources shared by the plugin and the test runner, so the tests exercise the shipped code.
set(GRAINLOCK_SOURCES
    Source/Parameters.cpp
    Source/Presets.cpp
    Source/dsp/GrainVoice.cpp
    Source/dsp/GrainEngine.cpp
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    Source/ui/GrainLookAndFeel.cpp
    Source/ui/Controls.cpp
    Source/ui/GrainDisplay.cpp
    Source/ui/LfoPanel.cpp
    Source/ui/MainPanel.cpp)

set(GRAINLOCK_DEFINITIONS
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0)

if(GRAINLOCK_BUILD_PLUGIN)
    juce_add_plugin(GrainLock
        COMPANY_NAME                "Kalan Hatton"
        COMPANY_WEBSITE             "https://github.com/kalanhattonre-bit/GrainLock"
        BUNDLE_ID                   com.kalanhatton.grainlock
        PLUGIN_MANUFACTURER_CODE    Khtn
        PLUGIN_CODE                 Grlk
        FORMATS                     VST3
        PRODUCT_NAME                "GrainLock"
        VERSION                     ${PROJECT_VERSION}
        IS_SYNTH                    FALSE
        NEEDS_MIDI_INPUT            TRUE
        NEEDS_MIDI_OUTPUT           FALSE
        IS_MIDI_EFFECT              FALSE
        EDITOR_WANTS_KEYBOARD_FOCUS FALSE
        COPY_PLUGIN_AFTER_BUILD     FALSE
        VST3_CATEGORIES             Fx)

    target_sources(GrainLock PRIVATE ${GRAINLOCK_SOURCES})
    target_include_directories(GrainLock PRIVATE Source)
    target_compile_definitions(GrainLock PUBLIC ${GRAINLOCK_DEFINITIONS})
    target_link_libraries(GrainLock
        PRIVATE
            juce::juce_audio_processors
            juce::juce_dsp
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_warning_flags)
endif()

if(GRAINLOCK_BUILD_TESTS)
    juce_add_console_app(GrainLockTests PRODUCT_NAME "GrainLockTests")

    target_sources(GrainLockTests PRIVATE Tests/TestMain.cpp ${GRAINLOCK_SOURCES})
    target_include_directories(GrainLockTests PRIVATE Source Tests)
    target_compile_definitions(GrainLockTests PRIVATE ${GRAINLOCK_DEFINITIONS} GRAINLOCK_TESTS=1)
    target_link_libraries(GrainLockTests
        PRIVATE
            juce::juce_audio_processors
            juce::juce_dsp
            juce::juce_recommended_config_flags
            juce::juce_recommended_warning_flags)
endif()
