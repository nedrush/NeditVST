#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"

//==============================================================================
/** Draws the loaded sample's waveform, a vertical line at each slice
    boundary (bright magenta = manually placed, white = auto-detected),
    one draggable probability "fader" per slice, a live highlight over
    whichever slice is currently sounding, two draggable Trim Start /
    Trim End handles (Step 23) — distinct yellow flagged lines, with
    everything outside them dimmed to make the excluded region obvious —
    a thin dodger-blue playhead line tracking Audition's current read
    position while it's running (Step 28) — a distinct colour from every
    other marker/cue here, and mutually exclusive with the generative
    playhead highlight above (Audition and the generative engine can
    never run at the same time), so no overlap handling is needed — and a
    small, unobtrusive beat-number grid (Step 31): a tick + number (1,
    2, 3...) at each of loopLengthBars*4 evenly-spaced positions across
    [trimStart, trimEnd), informational only, drawn only for beats that
    fall within the current visible range.

    Zoom/pan (Step 31): [visibleStartSample, visibleEndSample) is the
    window into the sample this component currently shows — defaults to
    the whole buffer (fully zoomed out), reset back to that whenever a
    genuinely new (different-length) sample loads. EVERY sample<->pixel
    conversion in this class goes through this range via xToSample()/
    sampleToX(), the two seams every caller already uses, so panning/
    zooming doesn't require touching interaction code elsewhere. Plain
    scroll zooms (in/out, centred on whatever sample is under the cursor);
    Shift+scroll pans; both are clamped to never leave [0, totalSamples]
    and never zoom in past minVisibleRangeSamples(). zoomToTrims()/
    resetZoom() are called by two editor buttons of the same name.

    Interactions:
      - Drag within a slice's body (away from any boundary line) sets that
        slice's probability (top = 1.0, bottom = 0.0).
      - Double-click in empty space adds a new manual slice point there,
        snapped to the nearest real transient-like peak.
      - Click-drag a manual point (magenta line) moves it, re-snapping live.
      - Shift + double-click, or Shift + drag a manual point, bypasses
        snapping entirely — free placement at the exact position.
      - Double-click or Cmd-click a manual point removes it.
      - Double-click or Cmd-click an auto-detected point (white line)
        deletes it — it won't come back until sensitivity or Reset changes
        that. Position 0 can't be deleted.
      - Click-drag a Trim Start/End handle (yellow flag) moves it, snapping
        to the nearest transient (Shift bypasses snapping, same as manual
        slice points — identical modifier behaviour, no new pattern to
        learn) — nothing outside the trimmed range can ever be detected,
        manually placed, or played; dragging a handle inward past an
        existing slice boundary drops that boundary from the active slice
        list on the next rebuild.
      - Scroll wheel zooms; Shift+scroll wheel pans (Step 31).

    Also accepts drag-and-drop of audio files straight onto it. */
class WaveformDisplay : public juce::Component,
                         public juce::FileDragAndDropTarget,
                         private juce::Timer
{
public:
    explicit WaveformDisplay (SlicerAudioProcessor& processorToUse);

    void paint (juce::Graphics& g) override;
    void resized() override;

    void mouseDown (const juce::MouseEvent& event) override;
    void mouseDrag (const juce::MouseEvent& event) override;
    void mouseUp (const juce::MouseEvent& event) override;
    void mouseWheelMove (const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel) override;

    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void fileDragEnter (const juce::StringArray& files, int x, int y) override;
    void fileDragExit (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;

    // Call after loading a sample or re-running detection (e.g. sensitivity
    // changed) so the cached waveform peaks and slice layout get rebuilt.
    // Also clears any live preview, since a real refresh means there's now
    // committed data to show instead.
    void refresh();

    // Live preview (Step 12): shows a proposed slice layout — e.g. while
    // the sensitivity slider is being dragged — without it being the
    // actual committed state. Probability faders and the playhead
    // highlight are suppressed while previewing, since slice indices in
    // the preview don't necessarily line up with committed ones.
    void showPreviewSlices (const std::vector<Slice>& preview);
    void clearPreviewSlices();

    // Zoom/pan (Step 31) — called by the editor's "Zoom to Trims"/"Reset
    // Zoom" buttons. Both no-ops if no sample is loaded.
    void zoomToTrims(); // [trimStart, trimEnd), plus a small margin so the handles themselves stay grabbable
    void resetZoom();   // back to [0, totalSamples) — fully zoomed out

    // Set by the editor — called after a drag-and-drop load, since that
    // bypasses the editor's own Load Sample button and its follow-up UI
    // updates (status text, BPM display).
    std::function<void()> onSampleChanged;

    // Set by the editor — called whenever either trim handle's position
    // actually changes (Step 23), from a drag or from snap-to-transient,
    // but NOT for a drag event that lands on the same (clamped) position
    // as before (Step 33) — so the "~X BPM" label (computeSourceSpanSeconds()
    // depends on the trim range) and slice-count status text stay live
    // while dragging, and the editor can flag Loop Length as needing a
    // fresh look (Step 33) without re-flagging it every frame for a value
    // that never moved.
    std::function<void()> onTrimChanged;

private:
    void timerCallback() override; // drives the live playhead highlight, audition playhead, and Cmd-hover cue

    void rebuildWaveformPeaks();
    int getSliceIndexAtX (int x) const; // -1 if no sample loaded or x is outside any slice
    int xToSample (int x) const;   // maps through [visibleStartSample, visibleEndSample), not the whole buffer
    float sampleToX (int sample) const; // inverse of xToSample -- may return a value outside [0, width) for an off-screen sample
    int findManualPointNear (int x) const; // -1 if no manual point is close enough to x
    int findAutoPointNear (int x) const;   // returns a SAMPLE POSITION (not an id), -1 if none close
    void setProbabilityFromMouse (const juce::MouseEvent& event);
    static bool isSupportedAudioFile (const juce::File& file);

    // Zoom/pan (Step 31) helpers.
    int minVisibleRangeSamples() const; // a few ms worth, derived from the sample's own rate -- the zoom-in floor
    void clampVisibleRange();           // keeps [visibleStartSample, visibleEndSample) valid after any change

    // Auto-pan while dragging (Step 32) -- called from mouseDrag() for the
    // trim-handle and manual-point-drag branches only, before that drag's
    // own xToSample(event.x) call, so a drag toward either edge of a
    // zoomed-in view scrolls the view to follow rather than stalling once
    // the mouse reaches the component boundary. Same underlying pan
    // mechanism as Shift+scroll (mouseWheelMove), just triggered by drag
    // proximity to an edge instead of a wheel notch, and by a smaller
    // amount per call since mouseDrag fires far more often than a wheel
    // notch does.
    void autoPanIfNearEdge (int x);

    // Trim markers (Step 23).
    enum class TrimHandle { none, start, end };
    TrimHandle findTrimHandleNear (int x) const;

    SlicerAudioProcessor& processor;

    // One {min, max} pair per horizontal pixel column, built at the
    // current component width — avoids re-scanning the whole sample
    // buffer on every paint call.
    std::vector<std::pair<float, float>> waveformPeaks;

    bool isDraggingOver = false;      // true while a file is hovering during drag-and-drop
    int draggingManualPointId = -1;   // -1 = not currently dragging a manual point
    int dragStartSamplePosition = -1; // where a manual point was before the current drag, for undo

    // Trim markers (Step 23) — which handle (if any) the current drag is
    // moving. Continuous parameter, not undo-tracked (same bucket as
    // sensitivity/loop length), so unlike draggingManualPointId there's no
    // start-position bookkeeping needed here.
    TrimHandle draggingTrimHandle = TrimHandle::none;

    std::vector<Slice> previewSlices;
    bool hasPreview = false;

    // Polled every timer tick (not driven by mouseMove) so it also reacts
    // to Cmd being pressed/released while the mouse stays still — a sample
    // position, matching whichever boundary (manual or auto) is currently
    // under the cursor with Cmd held. -1 = nothing hovered for deletion.
    int hoveredDeletableSamplePosition = -1;

    // Same idea, for Shift held over a manual point — indicates a drag
    // from here would bypass snapping (free placement). Auto-detected
    // points don't get this cue since they can't be dragged at all.
    int hoveredFreePlaceSamplePosition = -1;

    // Zoom/pan (Step 31) — the currently visible window into the sample,
    // in source-sample units. Set to [0, totalSamples) (fully zoomed out)
    // whenever refresh() detects a genuinely new sample (see
    // lastKnownTotalSamples below); otherwise preserved across every other
    // refresh() call (re-slicing, trim drags, manual edits — all of which
    // call refresh() too, but none of which should reset the user's
    // current view).
    int visibleStartSample = 0;
    int visibleEndSample = 0;

    // refresh()'s signal for "this is a new sample, not just a re-slice of
    // the current one" — the whole buffer's length only changes when a
    // genuinely different file loads (redetection/trim/manual edits all
    // operate within the same buffer). Not a perfect signal (a same-length
    // reload of a different file wouldn't be caught) but simple, robust,
    // and needs no extra plumbing from the processor.
    int lastKnownTotalSamples = 0;

    // Zoom/pan tuning constants (Step 31) — implementer's discretion, per
    // spec. minVisibleRangeMs is "a handful of milliseconds" so scrolling
    // can never zoom into single-sample nonsense; the other two just set
    // how much one wheel notch zooms/pans by.
    static constexpr float minVisibleRangeMs = 20.0f;
    static constexpr float zoomFactorPerNotch = 1.5f; // was 1.2f -- too sluggish, per feedback
    static constexpr float panFractionPerNotch = 0.15f;

    // Auto-pan-while-dragging tuning (Step 32). Threshold is in pixels
    // (not sample-dependent, unlike the pan amount itself) since it's
    // about how close the mouse is to the component's edge, regardless of
    // zoom level. The per-event fraction is much smaller than
    // panFractionPerNotch since mouseDrag fires far more often than a
    // single wheel notch.
    static constexpr int autoPanEdgeThresholdPixels = 20;
    static constexpr float autoPanFractionPerDragEvent = 0.02f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WaveformDisplay)
};
