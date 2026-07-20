#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"

//==============================================================================
/** Draws the loaded sample's waveform, a vertical line at each slice
    boundary (bright magenta = manually placed, white = auto-detected),
    one draggable probability "fader" per slice, and a live highlight over
    whichever slice is currently sounding.

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

    // Set by the editor — called after a drag-and-drop load, since that
    // bypasses the editor's own Load Sample button and its follow-up UI
    // updates (status text, BPM display).
    std::function<void()> onSampleChanged;

private:
    void timerCallback() override; // drives the live playhead highlight + Cmd-hover cue

    void rebuildWaveformPeaks();
    int getSliceIndexAtX (int x) const; // -1 if no sample loaded or x is outside any slice
    int xToSample (int x) const;
    int findManualPointNear (int x) const; // -1 if no manual point is close enough to x
    int findAutoPointNear (int x) const;   // returns a SAMPLE POSITION (not an id), -1 if none close
    void setProbabilityFromMouse (const juce::MouseEvent& event);
    static bool isSupportedAudioFile (const juce::File& file);

    SlicerAudioProcessor& processor;

    // One {min, max} pair per horizontal pixel column, built at the
    // current component width — avoids re-scanning the whole sample
    // buffer on every paint call.
    std::vector<std::pair<float, float>> waveformPeaks;

    bool isDraggingOver = false;      // true while a file is hovering during drag-and-drop
    int draggingManualPointId = -1;   // -1 = not currently dragging a manual point
    int dragStartSamplePosition = -1; // where a manual point was before the current drag, for undo

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

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WaveformDisplay)
};
