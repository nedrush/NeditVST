# Generative Slicer — Step 1: plugin shell + sample loading

What this does right now: loads a WAV/AIFF/FLAC file and plays the **whole
sample** back on any MIDI note (8-voice polyphony, one-shot, no pitch
tracking, no slicing yet). The goal of this step is just to prove the
plumbing — file loading, MIDI-triggered playback, and the editor — works
end to end in a real DAW before we add the interesting parts.

## Setting it up (Projucer, matching your Buzzer/Repeat workflow)

1. Open Projucer → **New Project** → **Plug-In Basic**.
2. Name it e.g. `GenerativeSlicer`.
3. In the project settings:
   - **Plugin is a Synth**: ✅ (this makes it an instrument, not an effect)
   - **Plugin MIDI Input**: ✅
   - **Plugin MIDI Output**: leave unchecked
   - **Formats**: VST3 (add AU too if you want it in Logic)
4. Delete the default `PluginProcessor.h/.cpp` and `PluginEditor.h/.cpp`
   that Projucer generates, and replace them with the four files here.
5. Save the project (this regenerates the Xcode project) and open in Xcode
   to build, or keep editing in VS Code and just re-trigger the Xcode
   build when you want to test.

If you'd rather skip Projucer entirely and go CMake-based (also fine, and
arguably tidier long-term) — say the word and I'll set up a `CMakeLists.txt`
that pulls in JUCE from your existing `/Applications/JUCE/` instead.

## Testing it

Build, load the VST3/AU into a track in Live as an instrument, load a
sample via the **Load Sample...** button, and play any MIDI note — you
should hear the whole sample from the start each time, polyphonic if you
hold multiple notes.

## What's next (Step 2)

Port the transient detector we built and validated in the Max/MSP chat —
envelope follower → derivative → adaptive threshold → peak-picking with
holdoff — into C++, run it on `sampleBuffer` after loading, and store the
resulting slice boundaries. Then:

- **Step 3**: map slices to MIDI notes (chromatic from a root key, à la
  Simpler) so each note plays a specific slice instead of the whole sample.
- **Step 4**: the basic probability layer — instead of a strict 1:1 note→slice
  map, each trigger has a chance of playing a different/random slice. The
  `triggerProbability` member is already sitting in `PluginProcessor.h`
  waiting for this.

We'll build and test each step before moving to the next, same as we did
with the JS prototype.
