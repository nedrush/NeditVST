#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"

//==============================================================================
/** Control mode's on-screen keyboard -- a real piano-style keyboard covering
    just the keyswitch + slice-trigger range around the current base note,
    replacing the old plain text list of keyswitch note names
    (controlKeyswitchLabels). Reuses juce::MidiKeyboardComponent for the
    piano-key geometry, same base-class pattern as PerformanceKeyboardPanel
    (see its own doc comment for why juce::MidiKeyboardState must be listed
    FIRST in the base list -- identical reasoning applies here), but purely
    a display/selection surface: mouseDownOnKey()/mouseDraggedToKey() always
    return false, so a click never triggers a real MIDI note-on.

    Two coloured washes are painted across the full key height, drawn UNDER
    the normal key painting via drawWhiteNote()/drawBlackNote() overrides so
    real black/white key geometry still reads clearly through them: one over
    [baseNote - numPlaybackStyleOptions, baseNote - 1] (the fixed keyswitch
    block -- see SlicerAudioProcessor::getControlKeyswitchNote()) and one
    over [baseNote, baseNote + numSlices - 1] (the slice-trigger range),
    making the split between the two visually obvious at a glance instead of
    relying on reading note names. Clicking a keyswitch key calls
    Source::selectKeyswitchStyle() -- exactly the old label rows' click-to-
    select behaviour, just routed through a key instead of a text row -- and
    the currently selected keyswitch key gets an extra white border on top
    of its wash so the active style-tab stays visible. Slice-range keys are
    purely informational; clicking one does nothing.

    resized() recomputes the visible note range and key width every time the
    base note or slice count changes (via refresh()), so the keyboard always
    fills its full width with exactly the relevant range -- never the whole
    128-note keyboard, and never scrolled/scaled to show irrelevant notes. */
class ControlKeyboardPanel : private juce::MidiKeyboardState,
                              public juce::MidiKeyboardComponent
{
public:
    struct Source
    {
        virtual ~Source() = default;
        virtual int getBaseNote() const = 0;
        virtual int getNumSlices() const = 0;
        virtual int getSelectedKeyswitchStyle() const = 0; // -1 = none
        virtual void selectKeyswitchStyle (int styleIndex) = 0;
    };

    explicit ControlKeyboardPanel (Source& sourceToUse);

    void resized() override;

    void drawWhiteNote (int midiNoteNumber, juce::Graphics&, juce::Rectangle<float> area,
                         bool isDown, bool isOver, juce::Colour lineColour, juce::Colour textColour) override;
    void drawBlackNote (int midiNoteNumber, juce::Graphics&, juce::Rectangle<float> area,
                         bool isDown, bool isOver, juce::Colour noteFillColour) override;
    bool mouseDownOnKey (int midiNoteNumber, const juce::MouseEvent&) override;
    bool mouseDraggedToKey (int midiNoteNumber, const juce::MouseEvent&) override;

    // Call whenever the base note, slice count, or selected keyswitch style
    // changes externally -- relayouts the visible range and repaints.
    void refresh();

    static int getPreferredHeight() { return 120; }

    static const juce::Colour keyswitchWashColour;
    static const juce::Colour sliceWashColour;

private:
    void paintRangeOverlay (int midiNoteNumber, juce::Graphics&, juce::Rectangle<float> area);

    Source& source;

    // Reentrancy guard for resized() -- JUCE's KeyboardComponentBase::
    // setKeyWidth() calls resized() (virtually, so it lands right back on
    // this override) whenever the width actually changes. Without this
    // guard, the trial-then-rescale width fit in resized() below calls
    // setKeyWidth() with a value that (almost always) differs from
    // whatever the previous reentrant call just set, so each level's call
    // triggers another -- unbounded recursion, not just a wasted extra
    // pass. While the guard is set, a reentrant call only runs the base
    // class's own key-position layout (still correct for whatever keyWidth
    // was just set), never repeats the range/width-fitting logic itself.
    bool isLayingOutRange = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ControlKeyboardPanel)
};
