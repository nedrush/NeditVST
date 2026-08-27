#pragma once

#include <JuceHeader.h>

//==============================================================================
/** A plain-text numeric field, scrubbed by clicking and dragging vertically
    (up = increase, down = decrease) -- the Ableton/Logic-style parameter
    field, deliberately NOT a slider: no track, no fill bar, just the
    number itself. There's no built-in juce::Slider style that matches
    "just text, vertical-drag-to-scrub, no bar graphic" -- LinearBar still
    paints a filled bar behind the text, which reads as a small fader --
    so this is a small hand-built component instead, consistent with how
    most of this UI (SegmentedButtonRow, SectionPanel, the various
    hand-painted panels) is already built rather than assembled from
    stock JUCE widgets.

    Drag sensitivity is a fixed number of pixels per `interval` step
    (pixelsPerStepNormal), independent of the value's total range, so a
    control spanning a handful of integers (Loop Length's 1-8 bars) and one
    spanning a continuous 0-1 range (Transient sensitivity) both feel like
    a deliberate, controllable drag rather than either being twitchy or
    needing a huge sweep. Holding Shift while dragging divides that
    sensitivity by fineDragDivisor for fine adjustment, the same "Shift for
    fine control" convention Ableton/Logic's own number fields use.

    Double-click opens an inline juce::TextEditor pre-filled with the exact
    current value for typing a precise number directly, Return commits,
    Escape cancels -- the same secondary path those DAWs' fields offer
    alongside drag-to-scrub. */
class DragNumberField : public juce::Component
{
public:
    DragNumberField();
    ~DragNumberField() override;

    void setRange (double newMin, double newMax, double newInterval);
    void setNumDecimalPlacesToDisplay (int places) { decimalPlaces = places; repaint(); }

    double getValue() const noexcept { return value; }
    void setValue (double newValue, juce::NotificationType notification = juce::sendNotification);

    // Matches juce::Slider::isMouseButtonDown()'s own purpose -- callers
    // (e.g. Transient sensitivity's onValueChange) use this to tell a live
    // drag-in-progress apart from a settled value, same convention already
    // used throughout this editor for juce::Slider.
    bool isMouseButtonDown() const noexcept { return dragging; }

    // Mirrors juce::Slider::onValueChange/onDragEnd's own shape/semantics --
    // onValueChange fires on every value change (including mid-drag, same
    // as a Slider firing continuously while dragging); onDragEnd fires once
    // when a drag ends OR a typed value is committed (treated as the same
    // "user is done touching this" moment the rest of this editor already
    // keys "acknowledged" behaviour off of, e.g. loopLengthNeedsAttention).
    std::function<void()> onValueChange;
    std::function<void()> onDragEnd;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;

private:
    double snapToInterval (double v) const;
    juce::String formattedText() const;
    void beginTextEdit();
    void commitTextEditorValue();

    double value = 0.0, minValue = 0.0, maxValue = 1.0, interval = 0.0;
    int decimalPlaces = 0;

    bool dragging = false;
    int dragStartY = 0;
    double dragStartValue = 0.0;

    std::unique_ptr<juce::TextEditor> textEditor; // only exists while typing an exact value

    static constexpr double pixelsPerStepNormal = 10.0;
    static constexpr double fineDragDivisor = 10.0; // Shift-drag sensitivity divisor

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DragNumberField)
};
