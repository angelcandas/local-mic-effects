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
    NEEDS_MIDI_INPUT TRUE
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
        ui/widgets.js
        ui/fxviews.js
        ui/fxpresets.js
        ui/daw.js
        ui/rhythm.js
        ui/tempo.js
        ui/patterns.js
        ui/patterns.css
        ui/browser.js
        ui/pianoroll.js
        ui/takes.js
        ui/looper.js
        ui/midilearn.js
        ui/scenes.js
        ui/soundcheck.js
        ui/setlist.js
        ui/daw.css
        ui/aiviews.css
        ui/aiviews.js
        ui/practice.js
        ui/practice.css
        libs/JUCE/modules/juce_gui_extra/native/typescript/webview-interop/dist/index.js)

# The MicFX instruments (Grand, Nova, ...): built into the app for MIDI tracks, and each
# one also a VST3/AU plugin of its own (below).
set(MICFX_INSTRUMENT_SOURCES
    src/instruments/InstrumentCore.h
    src/instruments/Fx.h
    src/instruments/InstrumentProcessor.cpp
    src/instruments/InstrumentProcessor.h
    src/instruments/InstrumentEditor.cpp
    src/instruments/InstrumentEditor.h
    src/instruments/Registry.cpp
    src/instruments/Registry.h
    src/instruments/Grand.cpp
    src/instruments/Nova.cpp
    src/instruments/Tines.cpp
    src/instruments/Drawbar.cpp
    src/instruments/Pluck.cpp
    src/instruments/Mallets.cpp
    src/instruments/Beat.cpp
    src/instruments/Kit.cpp)

set(MICFX_DSP_SOURCES
    ${MICFX_INSTRUMENT_SOURCES}
    src/analysis/Analysis.cpp
    src/analysis/Analysis.h
    src/analysis/Key.cpp
    src/analysis/Key.h
    src/analysis/Chords.cpp
    src/analysis/Chords.h
    src/analysis/MixAssistant.cpp
    src/analysis/MixAssistant.h
    src/analysis/Beats.cpp
    src/analysis/Beats.h
    src/analysis/Coach.cpp
    src/analysis/Coach.h
    src/analysis/Pitch.cpp
    src/analysis/Pitch.h
    src/analysis/TakeComp.cpp
    src/analysis/TakeComp.h
    src/Parameters.cpp
    src/Presets.cpp
    src/PluginProcessor.cpp
    src/Recorder.cpp
    src/PitchTap.h
    src/Practice.cpp
    src/Practice.h
    src/Looper.cpp
    src/Looper.h
    src/MidiMap.cpp
    src/MidiMap.h
    src/Scenes.cpp
    src/Scenes.h
    src/Setlist.cpp
    src/Setlist.h
    src/SoundCheck.cpp
    src/SoundCheck.h
    src/Multitrack.cpp
    src/daw/AudioPool.cpp
    src/daw/DawEngine.cpp
    src/daw/Edit.cpp
    src/daw/Patterns.cpp
    src/daw/Patterns.h
    src/daw/Exporter.cpp
    src/daw/Instruments.cpp
    src/daw/PluginHost.cpp
    src/daw/Project.cpp
    src/daw/Quantize.cpp
    src/daw/Quantize.h
    src/daw/Warp.cpp
    src/daw/TempoMap.cpp
    src/daw/Warp.h
    src/dsp/Biquad.h
    src/dsp/Effects.h
    src/dsp/FxChain.h
    src/dsp/FxModule.h
    src/dsp/FxModules.h
    src/dsp/PitchDetector.h
    src/dsp/ParametricEq.h
    src/dsp/PitchEngine.h
    src/dsp/PsolaShifter.h
    src/stems/StemSplitter.cpp
    src/stems/StemSplitter.h)

# Stem splitting (Split into stems in DAW mode) runs Demucs with demucs.cpp (MIT) on
# Eigen (MPL 2.0), both git submodules. Without them the app still builds; the split
# button then says it isn't available in this build.
set(MICFX_DEMUCS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/libs/demucs.cpp" CACHE PATH "demucs.cpp checkout")
set(MICFX_EIGEN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/libs/eigen" CACHE PATH "Eigen checkout")

set(MICFX_HAS_COREML 0)

if(EXISTS "${MICFX_DEMUCS_DIR}/src/model.hpp" AND EXISTS "${MICFX_EIGEN_DIR}/Eigen/Dense")
    file(GLOB MICFX_DEMUCS_LIB_SOURCES CONFIGURE_DEPENDS "${MICFX_DEMUCS_DIR}/src/*.cpp")
    add_library(MicFXDemucs STATIC ${MICFX_DEMUCS_LIB_SOURCES})
    target_include_directories(MicFXDemucs SYSTEM PUBLIC "${MICFX_DEMUCS_DIR}/src" "${MICFX_EIGEN_DIR}")
    target_compile_features(MicFXDemucs PUBLIC cxx_std_17)
    set_target_properties(MicFXDemucs PROPERTIES POSITION_INDEPENDENT_CODE ON)
    # Eigen is far too slow unoptimised, so this library is optimised even in Debug
    # builds (a 4-minute song would take hours). Its own warnings are not ours to fix.
    target_compile_definitions(MicFXDemucs PRIVATE NDEBUG EIGEN_NO_DEBUG)
    if(MSVC)
        target_compile_options(MicFXDemucs PRIVATE /O2 /W0)
    else()
        target_compile_options(MicFXDemucs PRIVATE -O3 -fno-math-errno -w)
    endif()

    # Nearly all of Demucs is matrix multiplication (the convolutions are im2col + GEMM).
    # Eigen's own kernels are far slower than Apple's Accelerate, which runs on the M-series
    # matrix units; EIGEN_USE_BLAS hands every large product to it. (PUBLIC so DemucsSeparator,
    # which also includes Eigen, agrees with the library.) Elsewhere Eigen's kernels stay.
    if(APPLE)
        target_compile_definitions(MicFXDemucs PUBLIC EIGEN_USE_BLAS)
        target_link_libraries(MicFXDemucs PUBLIC "-framework Accelerate")
    endif()

    list(APPEND MICFX_DSP_SOURCES src/stems/DemucsSeparator.cpp src/stems/DemucsPipeline.cpp src/stems/DemucsPipeline.h)

    # On a Mac the network can also run on the GPU through Core ML (a model the user converts
    # once with scripts/export_htdemucs_coreml.py). The Objective-C++ adapter needs the system
    # frameworks, which every target that links the demucs library inherits.
    if(APPLE)
        list(APPEND MICFX_DSP_SOURCES src/stems/CoreMlSeparator.mm)
        set_source_files_properties(src/stems/CoreMlSeparator.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
        target_link_libraries(MicFXDemucs PUBLIC "-framework CoreML" "-framework Foundation")
        set(MICFX_HAS_COREML 1)
    endif()
    set(MICFX_DEMUCS_LIBRARY MicFXDemucs)
    set(MICFX_HAS_DEMUCS 1)
    message(STATUS "MicFX: stem splitting enabled (demucs.cpp)")
else()
    set(MICFX_DEMUCS_LIBRARY "")
    set(MICFX_HAS_DEMUCS 0)
    message(STATUS "MicFX: stem splitting disabled (run: git submodule update --init libs/demucs.cpp libs/eigen)")
endif()

target_sources(MicFX
    PRIVATE
        ${MICFX_DSP_SOURCES}
        src/DawBridge.cpp
        src/PluginEditor.cpp
        src/StandaloneApp.cpp)

target_include_directories(MicFX PRIVATE src)

target_compile_definitions(MicFX
    PUBLIC
        JUCE_WEB_BROWSER=1
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        # MIDI tracks in DAW mode host instrument plugins (src/daw/PluginHost).
        JUCE_PLUGINHOST_VST3=1
        JUCE_PLUGINHOST_AU=1
        # Our own standalone app (src/StandaloneApp.cpp) adds "Check for updates..." to the Options menu.
        JUCE_USE_CUSTOM_PLUGIN_STANDALONE_APP=1
    PRIVATE
        # Where scripts/update.sh lives, for "Check for updates...".
        MICFX_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
        MICFX_HAS_DEMUCS=${MICFX_HAS_DEMUCS}
        MICFX_HAS_COREML=${MICFX_HAS_COREML}
        # Debug builds load ui/ from disk so UI edits only need a page reload.
        $<$<CONFIG:Debug>:MICFX_UI_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/ui">
        $<$<CONFIG:Debug>:MICFX_JUCE_JS_FILE="${CMAKE_CURRENT_SOURCE_DIR}/libs/JUCE/modules/juce_gui_extra/native/typescript/webview-interop/dist/index.js">)

# JUCE's recommended warnings include a few that only add noise here: -Wfloat-equal fires on
# deliberate "did this parameter change?" caches and smoother snaps (exact compares of stored
# values, not computed ones), -Wmissing-field-initializers on partly filled Spec structs and
# -Wshadow on lambda/member names. Linked after the JUCE flags so these win.
add_library(micfx_quiet_warnings INTERFACE)
if(NOT MSVC)
    target_compile_options(micfx_quiet_warnings INTERFACE
        -Wno-float-equal -Wno-missing-field-initializers -Wno-shadow)
endif()

target_link_libraries(MicFX
    PRIVATE
        MicFX_WebUI
        ${MICFX_DEMUCS_LIBRARY}
        juce::juce_audio_utils
        juce::juce_cryptography
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
        micfx_quiet_warnings)

# Each instrument as a VST3 (and AU on macOS) plugin of its own: "MicFX Grand", "MicFX Nova"...
#   cmake --build build/debug --target MicFXInstruments
option(MICFX_BUILD_INSTRUMENT_PLUGINS "Build the instruments as VST3/AU plugins" ON)

if(MICFX_BUILD_INSTRUMENT_PLUGINS)
    add_custom_target(MicFXInstruments)

    function(micfx_add_instrument_plugin key name code category)
        set(target "MicFX${name}")
        juce_add_plugin(${target}
            COMPANY_NAME "Angel Candas"
            BUNDLE_ID "com.angelcandas.micfx.${key}"
            PLUGIN_MANUFACTURER_CODE Angc
            PLUGIN_CODE ${code}
            FORMATS VST3 AU
            PRODUCT_NAME "MicFX ${name}"
            IS_SYNTH TRUE
            NEEDS_MIDI_INPUT TRUE
            NEEDS_MIDI_OUTPUT FALSE
            IS_MIDI_EFFECT FALSE
            EDITOR_WANTS_KEYBOARD_FOCUS FALSE
            VST3_CATEGORIES Instrument ${category}
            AU_MAIN_TYPE kAudioUnitType_MusicDevice)

        target_sources(${target} PRIVATE src/instruments/PluginEntry.cpp ${MICFX_INSTRUMENT_SOURCES})
        target_include_directories(${target} PRIVATE src)
        target_compile_definitions(${target}
            PUBLIC JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0
            PRIVATE MICFX_INSTRUMENT_KEY="${key}")
        target_link_libraries(${target}
            PRIVATE juce::juce_audio_utils
            PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_lto_flags juce::juce_recommended_warning_flags micfx_quiet_warnings)
        add_dependencies(MicFXInstruments ${target}_All)
    endfunction()

    micfx_add_instrument_plugin(grand Grand Mgrd Piano)
    micfx_add_instrument_plugin(nova Nova Mnov Synth)
    micfx_add_instrument_plugin(tines Tines Mtin Piano)
    micfx_add_instrument_plugin(drawbar Drawbar Mdrb Organ)
    micfx_add_instrument_plugin(pluck Pluck Mplk Synth)
    micfx_add_instrument_plugin(mallets Mallets Mmlt Synth)
    micfx_add_instrument_plugin(beat Beat Mbea Drum)
    micfx_add_instrument_plugin(kit Kit Mkit Drum)
endif()

# Headless test runner for the DSP: pitch accuracy, autotune, every preset, CPU load.
#   cmake --build --preset debug --target MicFXTests && ./build/debug/MicFXTests_artefacts/Debug/MicFXTests
option(MICFX_BUILD_TESTS "Build the DSP test runner" ON)

if(MICFX_BUILD_TESTS)
    # A tiny VST3 instrument the tests scan and load (plugin hosting for MIDI tracks).
    juce_add_plugin(MicFXTestSynth
        COMPANY_NAME "MicFX Tests"
        BUNDLE_ID "com.angelcandas.micfx.testsynth"
        PLUGIN_MANUFACTURER_CODE Mfxt
        PLUGIN_CODE Mfts
        FORMATS VST3
        PRODUCT_NAME "MicFX Test Synth"
        IS_SYNTH TRUE
        NEEDS_MIDI_INPUT TRUE
        VST3_CATEGORIES Instrument Synth)

    target_sources(MicFXTestSynth PRIVATE tests/TestSynth.cpp)
    target_compile_definitions(MicFXTestSynth PUBLIC JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0)
    target_link_libraries(MicFXTestSynth
        PRIVATE juce::juce_audio_utils
        PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)

    juce_add_console_app(MicFXTests PRODUCT_NAME "MicFXTests")
    add_dependencies(MicFXTests MicFXTestSynth_VST3)

    target_sources(MicFXTests PRIVATE tests/DspTests.cpp src/DawBridge.cpp ${MICFX_DSP_SOURCES})
    target_include_directories(MicFXTests PRIVATE src)

    target_compile_definitions(MicFXTests
        PRIVATE
            JucePlugin_Name="MicFX"
            MICFX_HAS_DEMUCS=${MICFX_HAS_DEMUCS}
            MICFX_HAS_COREML=${MICFX_HAS_COREML}
            MICFX_TEST_SYNTH="$<GENEX_EVAL:$<TARGET_PROPERTY:MicFXTestSynth_VST3,JUCE_PLUGIN_ARTEFACT_FILE>>"
            JUCE_PLUGINHOST_VST3=1
            JUCE_PLUGINHOST_AU=1
            JUCE_MODAL_LOOPS_PERMITTED=1
            JUCE_WEB_BROWSER=0
            JUCE_USE_CURL=0)

    target_link_libraries(MicFXTests
        PRIVATE
            juce::juce_audio_processors
            juce::juce_audio_utils
            juce::juce_audio_formats
            juce::juce_cryptography
            juce::juce_dsp
            ${MICFX_DEMUCS_LIBRARY}
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_warning_flags
            micfx_quiet_warnings)

    # JUCE_PLUGINHOST_AU=1 pulls in AUGenericView, which lives in CoreAudioKit.
    if(APPLE)
        target_link_libraries(MicFXTests PRIVATE "-framework CoreAudioKit")
    endif()
endif()
