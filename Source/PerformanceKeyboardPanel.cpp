#include "PerformanceKeyboardPanel.h"

#if JUCE_DEBUG
#include <iostream> // TEMPORARY DEBUG (Performance mode freeze investigation)
#endif

PerformanceKeyboardPanel::PerformanceKeyboardPanel (Source& sourceToUse)
    : juce::MidiKeyboardState(),
      juce::MidiKeyboardComponent (*this, juce::MidiKeyboardComponent::horizontalKeyboard),
      source (sourceToUse)
{
    setAvailableRange (0, 127);
    setScrollButtonsVisible (false); // resized() always fits the full range exactly -- see below
    setWantsKeyboardFocus (false); // click-to-focus only -- never let QWERTY keys play/select through the base class

    populatedSlots = source.getPopulatedSlots();
    focusedSlot = source.getFocusedSlot();

    pollTimer.startTimerHz (15); // same poll-and-resync reasoning as PatternBankPanel
}

PerformanceKeyboardPanel::~PerformanceKeyboardPanel()
{
    pollTimer.stopTimer();
}

void PerformanceKeyboardPanel::resized()
{
    // See isFittingWidth's own doc comment -- setKeyWidth() below can
    // reenter this exact function; while that's happening, only run the
    // base class's own key-position layout, never repeat the fitting logic.
    if (isFittingWidth)
    {
        juce::MidiKeyboardComponent::resized();
        return;
    }

    isFittingWidth = true;

    // Two-pass width fit: lay out at a trial key width, measure the total
    // keyboard width that produced, then scale so the real width exactly
    // fills getWidth() -- getTotalKeyboardWidth() depends on keyWidth
    // already being set, so there's no closed-form way to solve for it
    // directly for an arbitrary mix of black/white keys in range.
    constexpr float trialKeyWidth = 20.0f;
    setKeyWidth (trialKeyWidth);
    const float totalAtTrial = getTotalKeyboardWidth();
    if (totalAtTrial > 0.0f && getWidth() > 0)
        setKeyWidth (trialKeyWidth * (float) getWidth() / totalAtTrial);

    isFittingWidth = false;

    juce::MidiKeyboardComponent::resized(); // final, real layout pass at the now-settled keyWidth
}

bool PerformanceKeyboardPanel::mouseDownOnKey (int midiNoteNumber, const juce::MouseEvent&)
{
#if JUCE_DEBUG
    // TEMPORARY DEBUG (Performance mode freeze investigation) -- confirms
    // the click reached here at all, before anything that could block.
    std::cerr << "[UI] PerformanceKeyboardPanel::mouseDownOnKey(" << midiNoteNumber << ")" << std::endl;
#endif

    source.focusSlot (midiNoteNumber);

#if JUCE_DEBUG
    std::cerr << "[UI] PerformanceKeyboardPanel::mouseDownOnKey(" << midiNoteNumber << ") -- focusSlot() returned" << std::endl;
#endif

    populatedSlots = source.getPopulatedSlots();
    focusedSlot = source.getFocusedSlot();
    repaint();

    return false; // selection only -- never trigger an actual note-on
}

bool PerformanceKeyboardPanel::mouseDraggedToKey (int, const juce::MouseEvent&)
{
    return false; // a click selects one slot; dragging across keys must never thrash focus or play notes
}

void PerformanceKeyboardPanel::paintSlotOverlay (int midiNoteNumber, juce::Graphics& g, juce::Rectangle<float> area)
{
    if (midiNoteNumber < 0 || midiNoteNumber >= 128)
        return;

    if (populatedSlots[(size_t) midiNoteNumber])
    {
        g.setColour (juce::Colours::mediumseagreen.withAlpha (0.55f));
        g.fillRect (area);
    }

    if (midiNoteNumber == focusedSlot)
    {
        g.setColour (juce::Colours::white);
        g.drawRect (area, 2.0f);
    }
}

void PerformanceKeyboardPanel::drawWhiteNote (int midiNoteNumber, juce::Graphics& g, juce::Rectangle<float> area,
                                               bool isDown, bool isOver, juce::Colour lineColour, juce::Colour textColour)
{
    juce::MidiKeyboardComponent::drawWhiteNote (midiNoteNumber, g, area, isDown, isOver, lineColour, textColour);
    paintSlotOverlay (midiNoteNumber, g, area);
}

void PerformanceKeyboardPanel::drawBlackNote (int midiNoteNumber, juce::Graphics& g, juce::Rectangle<float> area,
                                               bool isDown, bool isOver, juce::Colour noteFillColour)
{
    juce::MidiKeyboardComponent::drawBlackNote (midiNoteNumber, g, area, isDown, isOver, noteFillColour);
    paintSlotOverlay (midiNoteNumber, g, area);
}

void PerformanceKeyboardPanel::pollSource()
{
    const auto newPopulated = source.getPopulatedSlots();
    const int newFocused = source.getFocusedSlot();

    if (newPopulated == populatedSlots && newFocused == focusedSlot)
        return;

    populatedSlots = newPopulated;
    focusedSlot = newFocused;
    repaint();
}
