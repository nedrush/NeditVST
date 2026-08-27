#pragma once

#include <JuceHeader.h>
#include <array>

//==============================================================================
/** Performance mode's on-screen keyboard -- a real piano-style keyboard
    covering the full MIDI note range (0-127), replacing the old MIDI-Learn
    "Save to..." button + arm-and-assign flow entirely. Clicking a key sets
    editing FOCUS to that slot (see Source::focusSlot()) and nothing else --
    it never plays audio by itself. Physical MIDI never changes focus (see
    SlicerAudioProcessor::handlePerformanceStateNoteOn()); this panel is the
    only way focus moves.

    Reuses juce::MidiKeyboardComponent for the piano-key geometry/scrolling
    rather than reinventing it, but:
      - overrides mouseDownOnKey() to call Source::focusSlot() and return
        false, so a click never triggers an actual MIDI note-on through the
        keyboard's own (otherwise-unused) MidiKeyboardState;
      - overrides mouseDraggedToKey() to return false too -- without this,
        dragging across keys after a mouseDown would still fire note-on/off
        through the base class's own logic even though mouseDownOnKey never
        does, since the two callbacks are independent hooks;
      - overrides drawWhiteNote()/drawBlackNote() to layer two highlights on
        top of the normal key painting: a filled tint for any slot with a
        saved state (populatedSlots, sourced from the processor's actual
        bank contents), and a bright border for whichever slot currently has
        focus -- deliberately distinct from each other, so "already saved"
        and "being edited right now" are never conflated even when they're
        the same key;
      - disables the QWERTY-computer-keyboard-plays-notes feature
        (setWantsKeyboardFocus (false)) -- irrelevant here and would
        otherwise bypass mouseDownOnKey()'s focus-only contract entirely.

    Polls the source on a timer, same reasoning as PatternBankPanel's own
    poll -- focus changes originate from this panel's own clicks today, but
    polling costs nothing and stays correct if that ever changes.

    Privately inherits juce::MidiKeyboardState (never fed real MIDI I/O --
    mouseDownOnKey()/mouseDraggedToKey() always return false, so no note
    ever actually reaches it; it exists only because MidiKeyboardComponent's
    constructor requires one) rather than holding it as a plain data member.
    This is deliberate, not stylistic: base classes always finish
    constructing before ANY of a derived class's own members do, regardless
    of initializer-list order, so a plain member passed by reference into
    MidiKeyboardComponent's constructor (as this class originally did)
    would still be unconstructed at that point -- MidiKeyboardComponent's
    constructor immediately calls state.addListener(this), which locks a
    juce::CriticalSection living inside that not-yet-constructed object.
    That's undefined behaviour, and it manifested as exactly the kind of bug
    its symptoms suggest: harmless in a fresh, lightly-used process (the
    JUCE Standalone build never showed it) but a permanent hung mutex lock
    -- silent, no crash, no report -- inside a host process with much
    dirtier memory to inherit garbage from (reproduced every time loaded
    into Ableton Live). Listing MidiKeyboardState FIRST in the base list
    below guarantees it's the first thing constructed, before
    MidiKeyboardComponent ever gets a reference to it. */
class PerformanceKeyboardPanel : private juce::MidiKeyboardState,
                                  public juce::MidiKeyboardComponent
{
public:
    // Everything this panel needs from Performance mode's state bank.
    struct Source
    {
        virtual ~Source() = default;

        // Moves editing focus to noteNumber -- auto-saves whatever was
        // being edited in the previously-focused slot, then loads the new
        // slot's saved state (or a fresh default, if it has none yet).
        virtual void focusSlot (int noteNumber) = 0;

        virtual std::array<bool, 128> getPopulatedSlots() const = 0;
        virtual int getFocusedSlot() const = 0; // -1 = nothing focused yet
    };

    explicit PerformanceKeyboardPanel (Source& sourceToUse);
    ~PerformanceKeyboardPanel() override;

    void resized() override;

    void drawWhiteNote (int midiNoteNumber, juce::Graphics&, juce::Rectangle<float> area,
                         bool isDown, bool isOver, juce::Colour lineColour, juce::Colour textColour) override;
    void drawBlackNote (int midiNoteNumber, juce::Graphics&, juce::Rectangle<float> area,
                         bool isDown, bool isOver, juce::Colour noteFillColour) override;
    bool mouseDownOnKey (int midiNoteNumber, const juce::MouseEvent&) override;
    bool mouseDraggedToKey (int midiNoteNumber, const juce::MouseEvent&) override;

    static int getPreferredHeight() { return 90; }

private:
    void paintSlotOverlay (int midiNoteNumber, juce::Graphics&, juce::Rectangle<float> area);
    void pollSource();

    // MidiKeyboardComponent already privately inherits juce::Timer itself
    // (for its own note-flash animation) -- inheriting it again here
    // directly would make startTimerHz()/stopTimer() ambiguous between the
    // two Timer base subobjects, so the poll lives on this separate owned
    // object instead.
    struct PollTimer : public juce::Timer
    {
        explicit PollTimer (PerformanceKeyboardPanel& ownerToUse) : owner (ownerToUse) {}
        void timerCallback() override { owner.pollSource(); }
        PerformanceKeyboardPanel& owner;
    };

    Source& source;
    PollTimer pollTimer { *this };
    std::array<bool, 128> populatedSlots {};
    int focusedSlot = -1;

    // Reentrancy guard for resized() -- see ControlKeyboardPanel's own
    // isLayingOutRange for why setKeyWidth()'s trial-then-rescale fit needs
    // this: KeyboardComponentBase::setKeyWidth() calls resized() again
    // (virtually, landing right back here) whenever the width it's given
    // actually changes.
    bool isFittingWidth = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PerformanceKeyboardPanel)
};
