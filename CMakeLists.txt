cmake_minimum_required(VERSION 3.22)

project(Bitrot VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "Universal binary")
endif()

# ---------------------------------------------------------------------------
# JUCE: use a local copy if you have one (-DBITROT_JUCE_PATH=...),
# otherwise download JUCE 8 automatically.
# ---------------------------------------------------------------------------
set(BITROT_JUCE_PATH "" CACHE PATH "Path to a local JUCE checkout (optional)")
option(BITROT_BUILD_PLUGIN "Build the plugin (needs JUCE)" ON)
option(BITROT_BUILD_TESTS "Build the offline render test and the footswitch test" ON)

if(BITROT_BUILD_PLUGIN)
    if(BITROT_JUCE_PATH)
        add_subdirectory(${BITROT_JUCE_PATH} JUCE)
    else()
        include(FetchContent)
        FetchContent_Declare(
            JUCE
            GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
            GIT_TAG 8.0.4
            GIT_SHALLOW TRUE)
        FetchContent_MakeAvailable(JUCE)
    endif()

    set(BITROT_FORMATS VST3 Standalone)
    if(APPLE)
        list(APPEND BITROT_FORMATS AU)
    endif()

    juce_add_plugin(Bitrot
        COMPANY_NAME "Kevin Kaye"
        COMPANY_WEBSITE ""
        BUNDLE_ID "uk.kevinkaye.bitrot"
        PLUGIN_MANUFACTURER_CODE Kkay
        PLUGIN_CODE Btrt
        FORMATS ${BITROT_FORMATS}
        PRODUCT_NAME "Bitrot"
        DESCRIPTION "Modulated monophonic harmonising PLL"
        IS_SYNTH FALSE
        NEEDS_MIDI_INPUT TRUE
        NEEDS_MIDI_OUTPUT FALSE
        IS_MIDI_EFFECT FALSE
        EDITOR_WANTS_KEYBOARD_FOCUS FALSE
        COPY_PLUGIN_AFTER_BUILD FALSE
        VST3_CATEGORIES Fx Distortion)

    juce_add_binary_data(BitrotData SOURCES Resources/RobotoCondensed-Bold.ttf)
    if(UNIX AND NOT APPLE)
        set_target_properties(BitrotData PROPERTIES POSITION_INDEPENDENT_CODE ON)
    endif()

    target_sources(Bitrot PRIVATE
        Source/PluginProcessor.cpp
        Source/PluginEditor.cpp
        Source/PedalControl.cpp
        Source/Keeper.cpp)

    target_include_directories(Bitrot PRIVATE Source Source/engine)

    target_compile_definitions(Bitrot PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_DISPLAY_SPLASH_SCREEN=0)

    target_link_libraries(Bitrot
        PRIVATE
            BitrotData
            juce::juce_audio_utils
            juce::juce_dsp
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_lto_flags
            juce::juce_recommended_warning_flags)

    # Footswitch test: drives the real processor with plain 127/0 MIDI messages
    if(BITROT_BUILD_TESTS)
        juce_add_console_app(bitrot_foot_test PRODUCT_NAME "bitrot_foot_test")
        target_sources(bitrot_foot_test PRIVATE
            Tests/FootTest.cpp
            Source/PluginProcessor.cpp
            Source/PluginEditor.cpp
            Source/PedalControl.cpp
            Source/Keeper.cpp)
        target_include_directories(bitrot_foot_test PRIVATE Source Source/engine)
        target_compile_definitions(bitrot_foot_test PRIVATE
            JUCE_WEB_BROWSER=0
            JUCE_USE_CURL=0
            JUCE_MODAL_LOOPS_PERMITTED=1
            JUCE_STANDALONE_APPLICATION=1
            JucePlugin_Name="Bitrot"
            JucePlugin_VersionString="0.1.0")
        target_link_libraries(bitrot_foot_test PRIVATE
            BitrotData
            juce::juce_audio_utils
            juce::juce_dsp
            juce::juce_recommended_config_flags)
    endif()
endif()

# ---------------------------------------------------------------------------
# Offline engine test (no JUCE needed): locking, intervals, sub, freeze, corruption
# ---------------------------------------------------------------------------
if(BITROT_BUILD_TESTS)
    add_executable(bitrot_render_test Tests/RenderTest.cpp)
    target_include_directories(bitrot_render_test PRIVATE Source/engine)
endif()
