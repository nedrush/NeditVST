cmake_minimum_required(VERSION 3.22)

project(NeditVST VERSION 1.0.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# CMake-based CI build, alongside (not replacing) the existing Projucer/
# Xcode project in NeditVST/NeditVST.jucer — that stays the Mac dev
# workflow exactly as it is today. This file exists so GitHub Actions can
# build VST3s for Windows/Linux/macOS without needing JUCE preinstalled on
# the runner or a Projucer-generated per-platform project checked in.
include(FetchContent)
FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.14 # matches the JUCE version installed at /Applications/JUCE locally (JUCE v8.0.14)
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(JUCE)

# Plugin identity below is copied exactly from NeditVST/NeditVST.jucer (and
# its Projucer-resolved defaults in NeditVST/JuceLibraryCode/JucePluginDefines.h)
# so the CMake-built VST3 has an identical identity to the existing
# Xcode-built one — same manufacturer/plugin codes, same bundle ID, same
# characteristics. None of these are new values.
#
# NOTE: the .jucer's pluginCharacteristicsValue is
# "pluginIsSynth,pluginWantsMidiIn" — i.e. NEEDS_MIDI_INPUT currently
# resolves to TRUE, even though PluginProcessor::acceptsMidi() returns
# false in code (MIDI was functionally removed in Step 6). That's an
# existing mismatch in the .jucer file, not something introduced here —
# copied as-is per "don't invent new values."
juce_add_plugin(NeditVST
    COMPANY_NAME "yourcompany"
    COMPANY_WEBSITE "www.yourcompany.com"
    BUNDLE_ID "com.yourcompany.NeditVST"
    PRODUCT_NAME "NeditVST"
    DESCRIPTION "NeditVST"
    PLUGIN_MANUFACTURER_CODE Manu
    PLUGIN_CODE Tpmh
    FORMATS VST3
    VST3_CATEGORIES "Instrument" "Synth"
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
)

target_sources(NeditVST
    PRIVATE
        Source/PluginProcessor.cpp
        Source/PluginProcessor.h
        Source/PluginEditor.cpp
        Source/PluginEditor.h
        Source/TransientDetector.cpp
        Source/TransientDetector.h
        Source/WaveformDisplay.cpp
        Source/WaveformDisplay.h
        Source/SubdivisionProbabilityGrid.cpp
        Source/SubdivisionProbabilityGrid.h
        Source/GranularStretcher.cpp
        Source/GranularStretcher.h
        Source/PlaybackStyleGrid.cpp
        Source/PlaybackStyleGrid.h
        Source/SequencerGrid.cpp
        Source/SequencerGrid.h
)

juce_generate_juce_header(NeditVST)

target_compile_definitions(NeditVST
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        # Matches <JUCEOPTIONS> in NeditVST.jucer exactly.
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_STRICT_REFCOUNTEDPOINTER=1
)

# Matches <MODULES> in NeditVST.jucer, minus juce_audio_plugin_client and
# juce_audio_processors_headless — both are handled internally by
# juce_add_plugin()'s FORMATS argument and are never linked directly in
# the CMake JUCE workflow (linking juce_audio_plugin_client manually here
# would duplicate what juce_add_plugin already wires up).
target_link_libraries(NeditVST
    PRIVATE
        juce::juce_audio_basics
        juce::juce_audio_devices
        juce::juce_audio_formats
        juce::juce_audio_processors
        juce::juce_audio_utils
        juce::juce_core
        juce::juce_data_structures
        juce::juce_dsp
        juce::juce_events
        juce::juce_graphics
        juce::juce_gui_basics
        juce::juce_gui_extra
)
