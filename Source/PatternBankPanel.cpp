#include "PatternBankPanel.h"

//==============================================================================
PatternBankPanel::BankKeyboard::BankKeyboard (PatternBankPanel& ownerToUse)
    : juce::MidiKeyboardState(),
      juce::MidiKeyboardComponent (*this, juce::MidiKeyboardComponent::horizontalKeyboard),
      owner (ownerToUse)
{
    setAvailableRange (0, 127);
    setScrollButtonsVisible (false); // resized() always fits the full range exactly -- see below
    setWantsKeyboardFocus (false); // display/hover only -- never let QWERTY keys play through the base class
}

void PatternBankPanel::BankKeyboard::resized()
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

void PatternBankPanel::BankKeyboard::paintSlotOverlay (int midiNoteNumber, juce::Graphics& g, juce::Rectangle<float> area)
{
    if (midiNoteNumber < 0 || midiNoteNumber >= 128)
        return;

    if (owner.populatedSlots[(size_t) midiNoteNumber])
    {
        g.setColour (juce::Colours::mediumseagreen.withAlpha (0.55f));
        g.fillRect (area);
    }

    if (midiNoteNumber == owner.activeSlot)
    {
        g.setColour (juce::Colours::white);
        g.drawRect (area, 2.0f);
    }

    // Pending switch (Pattern Switch Timing: Set Interval/End of Pattern) --
    // a dashed amber ring, deliberately a different colour AND stroke style
    // from the active key's solid white border, so a pattern that's merely
    // headed-toward is never mistaken for one that's already sounding.
    if (midiNoteNumber == owner.pendingSlot)
    {
        const auto pendingBounds = area.expanded (1.0f);
        juce::Path dashedRing;
        dashedRing.addRectangle (pendingBounds);

        float dashLengths[] = { 3.0f, 2.0f };
        juce::Path dashedPath;
        juce::PathStrokeType (1.5f).createDashedStroke (dashedPath, dashedRing, dashLengths, 2);

        g.setColour (juce::Colours::orange);
        g.fillPath (dashedPath);
    }
}

void PatternBankPanel::BankKeyboard::drawWhiteNote (int midiNoteNumber, juce::Graphics& g, juce::Rectangle<float> area,
                                                     bool isDown, bool isOver, juce::Colour lineColour, juce::Colour textColour)
{
    juce::MidiKeyboardComponent::drawWhiteNote (midiNoteNumber, g, area, isDown, isOver, lineColour, textColour);
    paintSlotOverlay (midiNoteNumber, g, area);
}

void PatternBankPanel::BankKeyboard::drawBlackNote (int midiNoteNumber, juce::Graphics& g, juce::Rectangle<float> area,
                                                     bool isDown, bool isOver, juce::Colour noteFillColour)
{
    juce::MidiKeyboardComponent::drawBlackNote (midiNoteNumber, g, area, isDown, isOver, noteFillColour);
    paintSlotOverlay (midiNoteNumber, g, area);
}

void PatternBankPanel::BankKeyboard::mouseMove (const juce::MouseEvent& event)
{
    owner.updateStatusLabelForHoveredNote (getNoteAndVelocityAtPosition (event.position).note);
}

void PatternBankPanel::BankKeyboard::mouseExit (const juce::MouseEvent&)
{
    owner.updateStatusLabelForHoveredNote (-1);
}

//==============================================================================
PatternBankPanel::PatternBankPanel (BankSource& sourceToUse)
    : source (sourceToUse)
{
    addAndMakeVisible (saveButton);
    saveButton.onClick = [this] { saveButtonClicked(); };

    statusLabel.setJustificationType (juce::Justification::centredLeft);
    statusLabel.setFont (juce::Font (juce::FontOptions (11.0f)));
    statusLabel.setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.7f));
    addAndMakeVisible (statusLabel);

    addAndMakeVisible (keyboard);

    populatedSlots = source.getPopulatedSlots();
    activeSlot = source.getActiveSlot();
    pendingSlot = source.getPendingSlot();
    learnArmed = source.isLearnArmed();
    updateSaveButtonText();
    updateStatusLabelForHoveredNote (-1);

    setSize (600, getPreferredHeight()); // real width comes from the editor's own layout (setBounds())
    startTimerHz (15); // slot state can change on its own (a note-on arriving), nothing else to trigger a repaint
}

PatternBankPanel::~PatternBankPanel()
{
    stopTimer();
}

void PatternBankPanel::resized()
{
    auto area = getLocalBounds();

    auto topRow = area.removeFromTop (saveRowHeight);
    saveButton.setBounds (topRow.removeFromLeft (saveButton.getBestWidthForHeight (saveRowHeight) + 20));
    topRow.removeFromLeft (10);
    statusLabel.setBounds (topRow);

    area.removeFromTop (rowGap);
    keyboard.setBounds (area);
}

void PatternBankPanel::updateSaveButtonText()
{
    saveButton.setButtonText (learnArmed ? "Play a note..." : "Save to...");
}

void PatternBankPanel::updateStatusLabelForHoveredNote (int noteNumber)
{
    if (noteNumber >= 0)
    {
        const auto name = juce::MidiMessage::getMidiNoteName (noteNumber, true, true, 3);
        const bool populated = populatedSlots[(size_t) noteNumber];
        statusLabel.setText (name + (populated ? " (saved)" : " (empty)"), juce::dontSendNotification);
        return;
    }

    if (learnArmed)
    {
        statusLabel.setText ("Play a note to save here", juce::dontSendNotification);
        return;
    }

    // Active and pending are independent facts (a switch can be pending
    // while a DIFFERENT pattern is still the one actually sounding) --
    // shown together, never conflated into one word.
    juce::String text;

    if (activeSlot >= 0)
        text << "Active: " << juce::MidiMessage::getMidiNoteName (activeSlot, true, true, 3);

    if (pendingSlot >= 0)
    {
        if (text.isNotEmpty())
            text << "  ";

        text << "Pending: " << juce::MidiMessage::getMidiNoteName (pendingSlot, true, true, 3);
    }

    statusLabel.setText (text, juce::dontSendNotification);
}

void PatternBankPanel::saveButtonClicked()
{
    if (learnArmed)
        source.cancelLearn();
    else
        source.armSave();

    learnArmed = source.isLearnArmed();
    updateSaveButtonText();
    updateStatusLabelForHoveredNote (-1);
    keyboard.repaint();
}

void PatternBankPanel::timerCallback()
{
    const auto newPopulated = source.getPopulatedSlots();
    const int newActiveSlot = source.getActiveSlot();
    const int newPendingSlot = source.getPendingSlot();
    const bool newLearnArmed = source.isLearnArmed();

    const bool changed = (newPopulated != populatedSlots) || (newActiveSlot != activeSlot)
                          || (newPendingSlot != pendingSlot) || (newLearnArmed != learnArmed);

    if (! changed)
        return;

    populatedSlots = newPopulated;
    activeSlot = newActiveSlot;
    pendingSlot = newPendingSlot;
    learnArmed = newLearnArmed;
    updateSaveButtonText();
    updateStatusLabelForHoveredNote (keyboard.getNoteAndVelocityAtPosition (keyboard.getMouseXYRelative().toFloat()).note);
    keyboard.repaint();
}
