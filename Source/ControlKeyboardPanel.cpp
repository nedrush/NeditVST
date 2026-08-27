#include "ControlKeyboardPanel.h"
#include "PlaybackStylePalette.h"

// Reuses two colours already established elsewhere in the style palette
// (Ping-Pong's purple, Bitcrush's limegreen) rather than inventing new
// named colours just for this wash -- "existing style palette colours for
// anything style-specific" (keyswitches ARE per-style).
const juce::Colour ControlKeyboardPanel::keyswitchWashColour = PlaybackStylePalette::getStyleColour (1);
const juce::Colour ControlKeyboardPanel::sliceWashColour     = PlaybackStylePalette::getStyleColour (6);

ControlKeyboardPanel::ControlKeyboardPanel (Source& sourceToUse)
    : juce::MidiKeyboardState(),
      juce::MidiKeyboardComponent (*this, juce::MidiKeyboardComponent::horizontalKeyboard),
      source (sourceToUse)
{
    setScrollButtonsVisible (false); // the visible range always exactly fits (see resized())
    setWantsKeyboardFocus (false);   // display/selection only -- never let QWERTY keys play through the base class
}

void ControlKeyboardPanel::resized()
{
    // See isLayingOutRange's own doc comment -- setKeyWidth() below can
    // reenter this exact function; while that's happening, only run the
    // base class's own key-position layout, never repeat the fitting logic.
    if (isLayingOutRange)
    {
        juce::MidiKeyboardComponent::resized();
        return;
    }

    isLayingOutRange = true;

    const int base = source.getBaseNote();
    const int rangeStart = juce::jlimit (0, 127, base - SlicerAudioProcessor::numPlaybackStyleOptions);
    const int rangeEnd = juce::jlimit (0, 127, base + juce::jmax (0, source.getNumSlices() - 1));

    setAvailableRange (rangeStart, juce::jmax (rangeStart, rangeEnd));

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

    setLowestVisibleKey (rangeStart);

    isLayingOutRange = false;

    juce::MidiKeyboardComponent::resized(); // final, real layout pass at the now-settled keyWidth
}

void ControlKeyboardPanel::refresh()
{
    resized();
    repaint();
}

void ControlKeyboardPanel::paintRangeOverlay (int midiNoteNumber, juce::Graphics& g, juce::Rectangle<float> area)
{
    const int base = source.getBaseNote();
    const int keyswitchStyle = base - 1 - midiNoteNumber;
    const bool isKeyswitch = keyswitchStyle >= 0 && keyswitchStyle < SlicerAudioProcessor::numPlaybackStyleOptions;
    const bool isSlice = ! isKeyswitch && midiNoteNumber >= base && midiNoteNumber < base + source.getNumSlices();

    if (isKeyswitch)
    {
        g.setColour (keyswitchWashColour.withAlpha (0.38f));
        g.fillRect (area);

        if (keyswitchStyle == source.getSelectedKeyswitchStyle())
        {
            g.setColour (juce::Colours::white);
            g.drawRect (area, 2.0f);
        }
    }
    else if (isSlice)
    {
        g.setColour (sliceWashColour.withAlpha (0.30f));
        g.fillRect (area);
    }
}

void ControlKeyboardPanel::drawWhiteNote (int midiNoteNumber, juce::Graphics& g, juce::Rectangle<float> area,
                                           bool isDown, bool isOver, juce::Colour lineColour, juce::Colour textColour)
{
    juce::MidiKeyboardComponent::drawWhiteNote (midiNoteNumber, g, area, isDown, isOver, lineColour, textColour);
    paintRangeOverlay (midiNoteNumber, g, area);
}

void ControlKeyboardPanel::drawBlackNote (int midiNoteNumber, juce::Graphics& g, juce::Rectangle<float> area,
                                           bool isDown, bool isOver, juce::Colour noteFillColour)
{
    juce::MidiKeyboardComponent::drawBlackNote (midiNoteNumber, g, area, isDown, isOver, noteFillColour);
    paintRangeOverlay (midiNoteNumber, g, area);
}

bool ControlKeyboardPanel::mouseDownOnKey (int midiNoteNumber, const juce::MouseEvent&)
{
    const int base = source.getBaseNote();
    const int keyswitchStyle = base - 1 - midiNoteNumber;

    if (keyswitchStyle >= 0 && keyswitchStyle < SlicerAudioProcessor::numPlaybackStyleOptions)
    {
        source.selectKeyswitchStyle (keyswitchStyle);
        repaint();
    }

    return false; // selection only -- never trigger an actual note-on
}

bool ControlKeyboardPanel::mouseDraggedToKey (int, const juce::MouseEvent&)
{
    return false;
}
