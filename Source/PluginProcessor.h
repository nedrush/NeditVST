#pragma once

#include <JuceHeader.h>
#include "TransientDetector.h"

//==============================================================================
// STEP 1 of the build: plugin shell + sample loading + whole-sample playback.
//
// No slicing yet — every MIDI note just retriggers the whole loaded sample
// from the start, at unity pitch. This gets the plumbing (file loading,
// MIDI-triggered playback, polyphony) proven and testable in a DAW before
// we port the transient detector and cut the sample into slices.
//
// A `triggerProbability` hook is already wired in (see noteOn handling in
// the .cpp) so that when we get to the generative layer, we're extending
// something real rather than retrofitting it.
//==============================================================================

class SlicerAudioProcessor;

//==============================================================================
/** A "sound" that matches any MIDI note/channel — standard JUCE Synthesiser
    pattern. Later, when we have real slices, we'll likely map specific
    note ranges to specific slices; for now, one sound covers everything. */
class SliceSound : public juce::SynthesiserSound
{
public:
    bool appliesToNote (int /*midiNoteNumber*/) override { return true; }
    bool appliesToChannel (int /*midiChannel*/) override { return true; }
};

//==============================================================================
/** A single playback voice. Reads directly from the processor's shared
    sample buffer. Multiple voices = polyphony (e.g. overlapping slice hits). */
class SliceVoice : public juce::SynthesiserVoice
{
public:
    explicit SliceVoice (SlicerAudioProcessor& ownerProcessor);

    bool canPlaySound (juce::SynthesiserSound* sound) override;
    void startNote (int midiNoteNumber, float velocity,
                     juce::SynthesiserSound* sound, int currentPitchWheelPosition) override;
    void stopNote (float velocity, bool allowTailOff) override;
    void pitchWheelMoved (int newPitchWheelValue) override;
    void controllerMoved (int controllerNumber, int newControllerValue) override;
    void renderNextBlock (juce::AudioBuffer<float>& outputBuffer,
                           int startSample, int numSamples) override;

private:
    SlicerAudioProcessor& processor;
    double samplePosition = 0.0;
    double playbackRatio = 1.0; // sampleRate conversion ratio (source -> host)
    int sliceEndSample = 0;     // stop playback here, not at the buffer's end
    bool isActive = false;
    float currentVelocity = 1.0f;
};

//==============================================================================
class SlicerAudioProcessor : public juce::AudioProcessor
{
public:
    SlicerAudioProcessor();
    ~SlicerAudioProcessor() override;

    //=== Standard AudioProcessor overrides ===
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    //=== Sample loading (called from the editor) ===
    void loadSample (const juce::File& file);
    bool hasSample() const { return sampleLoaded; }
    juce::String getLoadedFileName() const { return loadedFileName; }

    //=== Slicing (Step 2) ===
    // Re-runs peak-picking (cheap — analysis is already cached) with new
    // sensitivity/holdoff and stores the result. Called once automatically
    // after loadSample() with default settings; the editor can call this
    // again later to let the user adjust sensitivity interactively.
    void redetectSlices (float sensitivity, float holdoffMs);
    int getNumSlices() const { return (int) slices.size(); }
    Slice getSlice (int index) const { return slices[(size_t) index]; }
    const std::vector<Slice>& getSlices() const { return slices; }

    //=== Note-to-slice mapping (Step 3) ===
    // Simpler-style: rootNote plays slice 0, each semitone above it plays
    // the next slice along. Notes below the root, or far enough above it
    // that there's no corresponding slice, simply don't trigger anything.
    void setRootNote (int newRootNote) { rootNote = newRootNote; }
    int getRootNote() const { return rootNote; }

    // Returns the slice index for a MIDI note, or -1 if that note doesn't
    // map to any detected slice (out of range either side).
    int getSliceIndexForNote (int midiNoteNumber) const
    {
        const int index = midiNoteNumber - rootNote;

        if (index < 0 || index >= (int) slices.size())
            return -1;

        return index;
    }

    //=== Shared read access for voices ===
    const juce::AudioBuffer<float>& getSampleBuffer() const { return sampleBuffer; }
    double getSampleSampleRate() const { return sampleSampleRate; }
    juce::CriticalSection& getSampleLock() { return sampleLock; }

    //=== Generative hook (placeholder for the probability layer) ===
    // 0.0 = never trigger, 1.0 = always trigger. Used later once we have
    // multiple slices to choose between; for now it's unused by voices.
    std::atomic<float> triggerProbability { 1.0f };

private:
    juce::AudioFormatManager formatManager;
    juce::Synthesiser synth;

    juce::AudioBuffer<float> sampleBuffer;
    double sampleSampleRate = 44100.0;
    bool sampleLoaded = false;
    juce::String loadedFileName;
    juce::CriticalSection sampleLock; // guards sampleBuffer during loadSample()

    TransientDetector transientDetector;
    std::vector<Slice> slices;

    // Middle C by default — first slice sits under the note most people
    // reach for first. Change via setRootNote() once the editor grows a
    // control for it.
    int rootNote = 60;

    // Sensible starting defaults — moderate sensitivity, 30ms holdoff to
    // avoid double-triggering on a single drum hit's ringing tail. These'll
    // want to be user-facing parameters once the editor grows a sensitivity
    // slider (part of the playback/UI step, not this one).
    static constexpr float defaultSensitivity = 0.5f;
    static constexpr float defaultHoldoffMs = 30.0f;

    static constexpr int numVoices = 8;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SlicerAudioProcessor)
};
