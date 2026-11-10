cmake_minimum_required(VERSION 3.22)

# Must be set before project(). Universal (arm64 + x86_64) builds are enabled in the release preset.
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")

project(MicFX VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_subdirectory(libs/JUCE)

juce_add_plugin(MicFX
    COMPANY_NAME "Angel Candas"
    BUNDLE_ID "com.angelcandas.micfx"
    PLUGIN_MANUFACTURER_CODE Angc
    PLUGIN_CODE Mcfx
    FORMATS Standalone AU VST3
    PRODUCT_NAME "MicFX"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    NEEDS_WEB_BROWSER TRUE
    NEEDS_CURL FALSE
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "MicFX processes your microphone in real time to apply audio effects."
    VST3_CATEGORIES Fx
    AU_MAIN_TYPE kAudioUnitType_Effect)

# The web UI is embedded into the binary and served to the WebView by PluginEditor.
juce_add_binary_data(MicFX_WebUI
    NAMESPACE WebUI
    HEADER_NAME WebUIData.h
    SOURCES
        ui/index.html
        ui/styles.css
        ui/app.js
        ui/modules.js
        libs/JUCE/modules/juce_gui_extra/native/typescript/webview-interop/dist/index.js)

set(MICFX_DSP_SOURCES
    src/Parameters.cpp
    src/Presets.cpp
    src/PluginProcessor.cpp
    src/Recorder.cpp
    src/Multitrack.cpp
    src/daw/AudioPool.cpp
    src/daw/DawEngine.cpp
    src/daw/Edit.cpp
    src/dsp/Biquad.h
    src/dsp/Effects.h
    src/dsp/FxChain.h
    src/dsp/FxModule.h
    src/dsp/FxModules.h
    src/dsp/PitchDetector.h
    src/dsp/PitchEngine.h
    src/dsp/PsolaShifter.h)

target_sources(MicFX
    PRIVATE
        ${MICFX_DSP_SOURCES}
        src/PluginEditor.cpp
        src/StandaloneApp.cpp)

target_include_directories(MicFX PRIVATE src)

target_compile_definitions(MicFX
    PUBLIC
        JUCE_WEB_BROWSER=1
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        # Our own standalone app (src/StandaloneApp.cpp) adds "Check for updates..." to the Options menu.
        JUCE_USE_CUSTOM_PLUGIN_STANDALONE_APP=1
    PRIVATE
        # Where scripts/update.sh lives, for "Check for updates...".
        MICFX_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
        # Debug builds load ui/ from disk so UI edits only need a page reload.
        $<$<CONFIG:Debug>:MICFX_UI_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/ui">
        $<$<CONFIG:Debug>:MICFX_JUCE_JS_FILE="${CMAKE_CURRENT_SOURCE_DIR}/libs/JUCE/modules/juce_gui_extra/native/typescript/webview-interop/dist/index.js">)

target_link_libraries(MicFX
    PRIVATE
        MicFX_WebUI
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# Headless test runner for the DSP: pitch accuracy, autotune, every preset, CPU load.
#   cmake --build --preset debug --target MicFXTests && ./build/debug/MicFXTests_artefacts/Debug/MicFXTests
option(MICFX_BUILD_TESTS "Build the DSP test runner" ON)

if(MICFX_BUILD_TESTS)
    juce_add_console_app(MicFXTests PRODUCT_NAME "MicFXTests")

    target_sources(MicFXTests PRIVATE tests/DspTests.cpp ${MICFX_DSP_SOURCES})
    target_include_directories(MicFXTests PRIVATE src)

    target_compile_definitions(MicFXTests
        PRIVATE
            JucePlugin_Name="MicFX"
            JUCE_WEB_BROWSER=0
            JUCE_USE_CURL=0)

    target_link_libraries(MicFXTests
        PRIVATE
            juce::juce_audio_processors
            juce::juce_audio_formats
            juce::juce_dsp
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_warning_flags)
endif()
