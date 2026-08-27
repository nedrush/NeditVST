#pragma once

#include <JuceHeader.h>
#include <array>

//==============================================================================
/** A 128-note-indexed MIDI bank UI -- shows, at a glance, which slots are
    populated and which one is currently active, and provides the "Save
    to..." control that arms MIDI Learn for assigning whatever the caller's
    context currently holds to whichever note is played next.

    Slots are shown on a real piano keyboard (BankKeyboard, reusing the same
    juce::MidiKeyboardComponent approach as ControlKeyboardPanel/
    PerformanceKeyboardPanel) rather than an abstract grid of squares -- an
    empty key paints normally; a populated key gets a filled green wash; the
    active key also gets a bright white border; a slot with a switch pending
    against it gets its own amber dashed ring instead -- deliberately a
    different colour/style from the active key's solid white border, so
    "headed there" is never mistaken for "already there." Hovering a key
    updates the status label with that note's name, so identifying a slot
    doesn't require memorizing raw MIDI note numbers. Purely a display/
    hover surface -- clicking a key never triggers a note-on or otherwise
    acts as a shortcut for recall; saving still only happens via "Save
    to..." + playing a real note, matching the original grid's behaviour.

    Deliberately generic over WHAT gets saved/recalled -- everything specific
    to a slot's own contents (Sequencer patterns, Performance states, or any
    future context) lives entirely on the BankSource side; this class only
    ever asks BankSource for slot occupancy/active/pending state and to
    arm/cancel Learn. Today only the Sequencer pattern bank uses this
    (Performance mode's own bank is PerformanceKeyboardPanel instead, a
    click-to-focus keyboard rather than a MIDI-Learn-recall one).

    Polls the source on a timer (same reasoning as SequencerGrid's own 30fps
    poll -- the audio thread can populate/activate slots on its own, via an
    incoming note-on, with nothing else to push a repaint). */
class PatternBankPanel : public juce::Component,
                          private juce::Timer
{
public:
    // Everything this panel needs from whatever context it's showing a bank
    // for -- see class doc comment above. getPendingSlot() returns -1 for a
    // context with no quantized-switch concept.
    struct BankSource
    {
        virtual ~BankSource() = default;
        virtual void armSave() = 0;
        virtual void cancelLearn() = 0;
        virtual bool isLearnArmed() const = 0;
        virtual std::array<bool, 128> getPopulatedSlots() const = 0;
        virtual int getActiveSlot() const = 0;
        virtual int getPendingSlot() const = 0;
    };

    explicit PatternBankPanel (BankSource& sourceToUse);
    ~PatternBankPanel() override;

    void resized() override;

    static int getPreferredHeight() { return saveRowHeight + rowGap + keyboardHeight; }

private:
    void timerCallback() override;
    void saveButtonClicked();
    void updateSaveButtonText();
    void updateStatusLabelForHoveredNote (int noteNumber);

    // The keyboard itself -- a nested class rather than a standalone member
    // of PluginEditor (like ControlKeyboardPanel/PerformanceKeyboardPanel
    // are) because this panel owns the Save button/status label ALONGSIDE
    // it as one reusable unit; nested classes have access to the enclosing
    // class's private members (C++11+), so it reads owner's populated/
    // active/pending state directly rather than through a second interface.
    struct BankKeyboard : private juce::MidiKeyboardState,
                           public juce::MidiKeyboardComponent
    {
        explicit BankKeyboard (PatternBankPanel& ownerToUse);

        void resized() override;

        void drawWhiteNote (int midiNoteNumber, juce::Graphics&, juce::Rectangle<float> area,
                             bool isDown, bool isOver, juce::Colour lineColour, juce::Colour textColour) override;
        void drawBlackNote (int midiNoteNumber, juce::Graphics&, juce::Rectangle<float> area,
                             bool isDown, bool isOver, juce::Colour noteFillColour) override;
        bool mouseDownOnKey (int, const juce::MouseEvent&) override { return false; }
        bool mouseDraggedToKey (int, const juce::MouseEvent&) override { return false; }
        void mouseMove (const juce::MouseEvent& event) override;
        void mouseExit (const juce::MouseEvent& event) override;

        void paintSlotOverlay (int midiNoteNumber, juce::Graphics&, juce::Rectangle<float> area);

        PatternBankPanel& owner;

        // Reentrancy guard for resized() -- see ControlKeyboardPanel's own
        // isLayingOutRange for why setKeyWidth()'s trial-then-rescale fit
        // needs this: KeyboardComponentBase::setKeyWidth() calls resized()
        // again (virtually, landing right back here) whenever the width it's
        // given actually changes.
        bool isFittingWidth = false;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BankKeyboard)
    };

    BankSource& source;

    juce::TextButton saveButton { "Save to..." };
    juce::Label statusLabel;
    BankKeyboard keyboard { *this };

    std::array<bool, 128> populatedSlots {};
    int activeSlot = -1;
    int pendingSlot = -1;
    bool learnArmed = false;

    static constexpr int saveRowHeight = 26;
    static constexpr int rowGap = 6;
    static constexpr int keyboardHeight = 90;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PatternBankPanel)
};
