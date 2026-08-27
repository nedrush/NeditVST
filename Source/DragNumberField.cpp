#include "DragNumberField.h"
#include "NeditPalette.h"

DragNumberField::DragNumberField()
{
    setMouseCursor (juce::MouseCursor::UpDownResizeCursor); // hints the drag-to-scrub interaction
}

DragNumberField::~DragNumberField() = default;

void DragNumberField::setRange (double newMin, double newMax, double newInterval)
{
    minValue = newMin;
    maxValue = newMax;
    interval = newInterval;
    setValue (value, juce::dontSendNotification); // re-clamp/snap the current value into the new range
}

double DragNumberField::snapToInterval (double v) const
{
    v = juce::jlimit (minValue, maxValue, v);

    if (interval > 0.0)
        v = minValue + std::round ((v - minValue) / interval) * interval;

    return v;
}

void DragNumberField::setValue (double newValue, juce::NotificationType notification)
{
    const double snapped = snapToInterval (newValue);

    if (juce::approximatelyEqual (snapped, value))
        return;

    value = snapped;
    repaint();

    if (notification != juce::dontSendNotification && onValueChange)
        onValueChange();
}

juce::String DragNumberField::formattedText() const
{
    return juce::String (value, decimalPlaces);
}

void DragNumberField::paint (juce::Graphics& g)
{
    if (textEditor != nullptr)
        return; // the TextEditor draws its own contents while active

    auto bounds = getLocalBounds();

    g.setColour (juce::Colours::white.withAlpha (0.06f));
    g.fillRoundedRectangle (bounds.toFloat(), 3.0f);

    g.setColour (dragging ? NeditPalette::salmon : NeditPalette::textOnTungsten);
    g.setFont (juce::Font (juce::FontOptions (13.0f)));
    g.drawFittedText (formattedText(), bounds, juce::Justification::centred, 1);
}

void DragNumberField::resized()
{
    if (textEditor != nullptr)
        textEditor->setBounds (getLocalBounds());
}

void DragNumberField::mouseDown (const juce::MouseEvent& event)
{
    if (textEditor != nullptr)
        return;

    dragging = true;
    dragStartY = event.y;
    dragStartValue = value;
    repaint();
}

void DragNumberField::mouseDrag (const juce::MouseEvent& event)
{
    if (! dragging)
        return;

    const double stepPixels = event.mods.isShiftDown() ? pixelsPerStepNormal * fineDragDivisor
                                                         : pixelsPerStepNormal;
    const double stepSize = interval > 0.0 ? interval : (maxValue - minValue) / 100.0;
    const double deltaSteps = (double) (dragStartY - event.y) / stepPixels; // up = positive = increase

    setValue (dragStartValue + deltaSteps * stepSize);
}

void DragNumberField::mouseUp (const juce::MouseEvent&)
{
    if (! dragging)
        return;

    dragging = false;
    repaint();

    if (onDragEnd)
        onDragEnd();
}

void DragNumberField::mouseDoubleClick (const juce::MouseEvent&)
{
    beginTextEdit();
}

void DragNumberField::beginTextEdit()
{
    if (textEditor != nullptr)
        return;

    dragging = false; // a double-click's first click may have started a drag -- cancel it, no onDragEnd (nothing was committed)

    textEditor = std::make_unique<juce::TextEditor>();
    textEditor->setInputRestrictions (0, "0123456789.-");
    textEditor->setJustification (juce::Justification::centred);
    textEditor->setFont (juce::Font (juce::FontOptions (13.0f)));
    textEditor->setSelectAllWhenFocused (true);
    textEditor->setText (juce::String (value, decimalPlaces), juce::dontSendNotification);
    textEditor->onReturnKey = [this] { commitTextEditorValue(); };
    textEditor->onFocusLost = [this] { commitTextEditorValue(); };
    textEditor->onEscapeKey = [this] { textEditor.reset(); repaint(); };

    addAndMakeVisible (*textEditor);
    textEditor->setBounds (getLocalBounds());
    textEditor->grabKeyboardFocus();
}

void DragNumberField::commitTextEditorValue()
{
    if (textEditor == nullptr)
        return; // already committed/cancelled by a prior call this same round-trip (Return then focus-lost)

    const double parsed = textEditor->getText().getDoubleValue();
    textEditor.reset();

    setValue (parsed);
    repaint();

    if (onDragEnd)
        onDragEnd(); // a typed commit counts as "done touching this", same as a drag ending
}
