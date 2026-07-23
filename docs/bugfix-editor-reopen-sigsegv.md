# Bug Fix: SIGSEGV on Editor Reopen with Loaded Sample

## Summary

Closing and reopening the plugin editor window while a sample is loaded
crashes the host engine with a SIGSEGV.  Reproducible with any audio file
-- the sample size only affects how spectacular the failure is.

## Root Cause

When the editor is destroyed and a new one is created (e.g. user toggles
the plugin window in Bitwig), the `WaveformDisplay` component is
reconstructed with default zoom state:

    visibleStartSample = 0
    visibleEndSample   = 0   // (default member initialisers)

`refresh()` -- which sets `visibleEndSample` to the actual buffer length
-- is only called from `updateAfterSampleOrSliceChange()`, and that
function is never invoked during editor construction when a sample is
already loaded.  The constructor only wired it up as a callback for
future file drops and sensitivity drags.

With `visibleEndSample - visibleStartSample == 0`, the `sampleToX()`
helper computes:

    visibleRange = jmax(1, 0) = 1
    pixel = (sample / 1) * componentWidth

For a 15-million-sample file, a slice boundary at sample 15,089,700 maps
to pixel **13,580,730,000**.  Every `fillRect` / `drawRect` call in
`paint()` receives these astronomical coordinates, and JUCE's software
renderer (`EdgeTable::iterate`) crashes with SIGSEGV trying to allocate
or iterate over a multi-billion-pixel edge table.

## Crash Signature

    SIGNAL / CRASH: SIGSEGV
    EdgeTable::iterate → Graphics::drawRect → WaveformDisplay::paint
    → Component::paintEntireComponent → LinuxRepaintManager::performAnyPendingRepaintsNow
    → Timer::TimerThread::CallTimersMessage::messageCallback

## Fix

Two lines added to the end of `SlicerAudioProcessorEditor`'s
constructor (`PluginEditor.cpp:388-389`):

    if (processor.hasSample())
        updateAfterSampleOrSliceChange();

This sets the visible range to `[0, totalSamples)`, rebuilds the
waveform peak cache, and triggers the initial repaint -- exactly the
same path that runs after a file drag-and-drop or a new load.

## Files Changed

- `Source/PluginEditor.cpp` -- constructor early-init (2 lines)

## Testing

1. Load any audio file into NeditVST.
2. Close the editor window.
3. Reopen the editor window.
4. Verify the waveform renders correctly and no crash occurs.

Repeat with both small (< 1 MB) and large (> 100 MB) files.
