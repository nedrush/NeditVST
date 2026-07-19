#include "PluginProcessor.h"
#include "PluginEditor.h"

//==============================================================================
// SliceVoice
//==============================================================================
SliceVoice::SliceVoice (SlicerAudioProcessor& ownerProcessor)
    : processor (ownerProcessor)
{
}

bool SliceVoice::canPlaySound (juce::SynthesiserSound* sound)
{
    return dynamic_cast<SliceSound*> (sound) != nullptr;
}

void SliceVoice::startNote (int /*midiNoteNumber*/, float velocity,
                             juce::SynthesiserSound*, int /*currentPitchWheelPosition*/)
{
    const juce::ScopedLock sl (processor.getSampleLock());

    if (! processor.hasSample())
    {
        isActive = false;
        return;
    }

    samplePosition = 0.0;
    currentVelocity = velocity;

    const double hostSampleRate = getSampleRate();
    const double sourceSampleRate = processor.getSampleSampleRate();
    playbackRatio = (hostSampleRate > 0.0) ? (sourceSampleRate / hostSampleRate) : 1.0;

    isActive = true;
}

void SliceVoice::stopNote (float /*velocity*/, bool allowTailOff)
{
    // Step 1 behaviour: notes play the whole sample through regardless of
    // note-off (like a "one-shot" trigger). We still need to respond to
    // stopNote so the Synthesiser can steal/recycle the voice if the host
    // asks for it without tail-off.
    if (! allowTailOff)
    {
        isActive = false;
        clearCurrentNote();
    }
}

void SliceVoice::pitchWheelMoved (int) {}
void SliceVoice::controllerMoved (int, int) {}

void SliceVoice::renderNextBlock (juce::AudioBuffer<float>& outputBuffer,
                                   int startSample, int numSamples)
{
    if (! isActive)
        return;

    const juce::ScopedLock sl (processor.getSampleLock());
    const auto& source = processor.getSampleBuffer();

    const int sourceLength = source.getNumSamples();
    const int sourceChannels = source.getNumChannels();

    if (sourceLength == 0 || sourceChannels == 0)
    {
        isActive = false;
        clearCurrentNote();
        return;
    }

    const int outChannels = outputBuffer.getNumChannels();

    for (int i = 0; i < numSamples; ++i)
    {
        if (samplePosition >= (double) (sourceLength - 1))
        {
            isActive = false;
            clearCurrentNote();
            break;
        }

        // Linear interpolation between the two nearest source samples.
        const int idx0 = (int) samplePosition;
        const int idx1 = juce::jmin (idx0 + 1, sourceLength - 1);
        const float frac = (float) (samplePosition - (double) idx0);

        for (int outCh = 0; outCh < outChannels; ++outCh)
        {
            const int srcCh = juce::jmin (outCh, sourceChannels - 1);
            const float s0 = source.getSample (srcCh, idx0);
            const float s1 = source.getSample (srcCh, idx1);
            const float sample = s0 + frac * (s1 - s0);

            outputBuffer.addSample (outCh, startSample + i, sample * currentVelocity);
        }

        samplePosition += playbackRatio;
    }
}

//==============================================================================
// SlicerAudioProcessor
//==============================================================================
SlicerAudioProcessor::SlicerAudioProcessor()
    : AudioProcessor (BusesProperties()
                           .withOutput ("Output", juce::AudioChannelSet::stereo(), true))
{
    formatManager.registerBasicFormats();

    for (int i = 0; i < numVoices; ++i)
        synth.addVoice (new SliceVoice (*this));

    synth.addSound (new SliceSound());
}

SlicerAudioProcessor::~SlicerAudioProcessor() = default;

void SlicerAudioProcessor::prepareToPlay (double sampleRate, int /*samplesPerBlock*/)
{
    synth.setCurrentPlaybackSampleRate (sampleRate);
}

void SlicerAudioProcessor::releaseResources() {}

bool SlicerAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    return layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

void SlicerAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;
    buffer.clear();

    synth.renderNextBlock (buffer, midiMessages, 0, buffer.getNumSamples());
}

juce::AudioProcessorEditor* SlicerAudioProcessor::createEditor()
{
    return new SlicerAudioProcessorEditor (*this);
}

void SlicerAudioProcessor::getStateInformation (juce::MemoryBlock& /*destData*/)
{
    // TODO once we have parameters worth persisting (slice points, mapping,
    // probability settings) — step 1 has nothing to save yet.
}

void SlicerAudioProcessor::setStateInformation (const void* /*data*/, int /*sizeInBytes*/)
{
}

void SlicerAudioProcessor::loadSample (const juce::File& file)
{
    std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (file));

    if (reader == nullptr)
        return;

    juce::AudioBuffer<float> newBuffer ((int) reader->numChannels, (int) reader->lengthInSamples);
    reader->read (&newBuffer, 0, (int) reader->lengthInSamples, 0, true, true);

    {
        const juce::ScopedLock sl (sampleLock);
        sampleBuffer = std::move (newBuffer);
        sampleSampleRate = reader->sampleRate;
        sampleLoaded = true;
        loadedFileName = file.getFileName();

        // Analysis is a one-off pass over the whole file — do it while we
        // still hold the lock so a voice can't start reading sampleBuffer
        // mid-swap on the audio thread.
        transientDetector.analyze (sampleBuffer, sampleSampleRate);
    }

    redetectSlices (defaultSensitivity, defaultHoldoffMs);
}

void SlicerAudioProcessor::redetectSlices (float sensitivity, float holdoffMs)
{
    auto newSlices = transientDetector.detectSlices (sensitivity, holdoffMs);

    const juce::ScopedLock sl (sampleLock);
    slices = std::move (newSlices);
}

//==============================================================================
// This creates new instances of the plugin.
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new SlicerAudioProcessor();
}
