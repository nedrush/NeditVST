#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"

//==============================================================================
/** Slice Length/Clock mode global-default parameter panel -- a style selector
    (juce::ComboBox, same "Forward"/"Ping-Pong"/... order as
    PlaybackStylePalette/PlaybackStyleGrid) sits above a single fixed-height
    row of compact rotary dials, one per parameter
    SlicerAudioProcessor::getApplicableSequencerCellParameters() says the
    selected style actually uses (Subdivide/Volume excluded -- see
    buildColumnsForStyle()'s own doc comment).

    Each dial shows the parameter's name above it and, beneath it, either its
    current Sweep Mode ("Static"/"Sweep In"/"Sweep Out", for a swept
    parameter -- Sample Rate Reduction/Bit Depth/Delay Time/Mix/Feedback),
    its current option name (a plain discrete parameter like Filter Type or
    Curve Shape), or its formatted numeric value (a plain continuous
    parameter like Resonance). Dragging a dial vertically adjusts its value;
    clicking a plain-discrete dial, or the Mode text beneath a swept one,
    opens the same juce::PopupMenu-of-named-options
    SequencerGrid::showParameterMenuForCell() uses for its own discrete
    submenus, parented to getTopLevelComponent() for the same reason (Rate's
    20-item list would otherwise get squashed into columns by a narrow
    parent).

    Because every style's dials sit in ONE row rather than a stack, the
    panel's height never depends on which style is selected or how many
    dials it has -- see getPreferredHeight()'s own doc comment. A style with
    fewer parameters (Tape Stop's single Curve Shape) just leaves the row's
    right side empty rather than shrinking the panel, matching this app's
    general "fixed-height container sized to the worst case" layout
    principle for every variable-content area.

    Unlike SequencerGrid, this panel reads/writes the GLOBAL default value for
    each parameter (SlicerAudioProcessor::getSequencerCellParameterGlobalValue()/
    setSequencerCellParameterGlobalValue()), not a per-step override -- the
    same storage a Sequencer step without its own override already falls back
    to, so editing here changes both Slice Length/Clock mode playback AND
    whatever Sequenced-mode steps have no override of their own.

    The style selector's current selection is purely local UI state for the
    default (Slice Length/Clock) construction, kept deliberately separate
    from SlicerAudioProcessor::getSelectedDrawingStyle() (Sequenced mode's
    "what style paints next" concept) -- reusing that value here would mean
    switching styles in this panel also changed what Sequenced mode's own
    grid draws with, and vice versa.

    Reused by Performance mode (Pass 1) via the optional constructor
    parameters below: a getValue/setValue pair lets a caller point this panel
    at ANY per-index float storage instead of the global default value --
    Performance mode passes lambdas bound to
    SlicerAudioProcessor::getPerformanceWorkingParameterValue()/
    setPerformanceWorkingParameterValue() instead, so editing there can never
    read or write the same global values Slice Length/Clock use (they must
    stay fully independent). initialStyleId/onStyleChanged make the style
    selector's own selection persist as real state too, when a caller needs
    that (Performance mode does; Slice Length/Clock's own instance, passing
    neither, keeps today's "purely local UI state" behaviour unchanged). */
class PlaybackStyleParameterPanel : public juce::Component
{
public:
    // getValue/setValue default to the global default value methods when
    // left null (the original Slice Length/Clock behaviour, unchanged).
    // initialStyleId seeds the style selector (JUCE's 1-based item IDs,
    // matching styleSelector.setSelectedId()'s own convention -- id 1 ==
    // style index 0). onStyleChanged, when set, is called with the newly
    // selected style index every time the selector changes -- see class doc
    // comment for why the default (null) instance leaves this purely local.
    using GetParameterValue = std::function<float (int paramIndex)>;
    using SetParameterValue = std::function<void (int paramIndex, float value)>;

    explicit PlaybackStyleParameterPanel (SlicerAudioProcessor& processorToUse,
                                           GetParameterValue getValueIn = nullptr,
                                           SetParameterValue setValueIn = nullptr,
                                           int initialStyleId = 1,
                                           std::function<void (int style)> onStyleChangedIn = nullptr);

    void paint (juce::Graphics&) override;
    void resized() override;

    void mouseDown (const juce::MouseEvent& event) override;
    void mouseDrag (const juce::MouseEvent& event) override;
    void mouseUp (const juce::MouseEvent& event) override;

    // Fixed height for the style selector row plus exactly one row of
    // dials -- unlike the old stacked-rows layout, this no longer varies
    // per style (every style's dials fit in the same single row, however
    // many or few there are), so every caller can reserve this once and
    // never reflow when the selected style changes.
    static int getPreferredHeight();

    // Hides (or reshows) the panel's own internal style-picker ComboBox
    // (Pass 1) -- for a caller that drives style selection with its own
    // external control instead (the Generate page's playbackStyleSegments,
    // a SegmentedButtonRow, via setSelectedStyle() below), so the two
    // pickers don't sit redundantly on top of each other. Purely a
    // visibility toggle -- row layout/getPreferredHeight() are unchanged,
    // still reserve the same top strip regardless, so hiding this doesn't
    // reflow anything else in the panel.
    void setStyleSelectorVisible (bool shouldBeVisible) { styleSelector.setVisible (shouldBeVisible); }

    // Resyncs the style selector to styleIndex without firing onStyleChanged
    // -- needed by a caller whose underlying value can change out from under
    // this panel (Performance mode's working state can change via a MIDI
    // recall, on the audio thread, same class of "poll and resync" issue
    // SlicerAudioProcessorEditor::timerCallback() already handles for
    // stepResolutionSelector/patternLengthSelector). A no-op if styleIndex
    // is already selected, so it's cheap to call unconditionally every tick.
    void setSelectedStyle (int styleIndex);

private:
    // One dial column: paramIndex is always the VALUE index (continuous, or
    // the option index for a plain discrete parameter); modeParamIndex is
    // paramIndex+1 for a swept parameter (its own Static/Sweep In/Sweep Out
    // Mode) and -1 otherwise; discreteSelect marks a plain discrete
    // parameter (Filter Type, Curve Shape, Rate, Forward/Backward Curve)
    // whose dial is click-to-open-a-menu rather than drag-to-adjust.
    struct DialColumn
    {
        int paramIndex;
        int modeParamIndex;
        bool discreteSelect;
    };

    // The three sub-rectangles one dial column paints/hit-tests against:
    // the parameter name above the dial, the dial itself, and the mode/
    // option/value text below it.
    struct ColumnLayout
    {
        juce::Rectangle<int> nameBounds, dialBounds, indicatorBounds;
    };

    // Builds the column list for one style: getApplicableSequencerCellParameters()
    // minus Subdivide (index 5) and Volume (index 19) -- both general
    // rather than style-specific, and neither has a global default dial
    // (getSequencerCellParameterGlobalValue() hardcodes their fallback);
    // Subdivide is a per-step retrigger rate tied to the step sequencer's
    // own step timing, Volume a per-step gain ramp keyed to the Sequencer's
    // Whole Window step timing (see PluginProcessor.h's own doc comment on
    // isSequencerCellParameterSwept()) -- neither is meaningful in Slice
    // Length/Clock modes.
    static std::vector<DialColumn> buildColumnsForStyle (int style);

    ColumnLayout getColumnLayout (int columnIndex) const;
    void showOptionsMenu (int paramIndex, juce::Rectangle<int> targetBounds);
    void drawDial (juce::Graphics& g, juce::Rectangle<int> dialBounds, float t, bool active) const;

    SlicerAudioProcessor& processor;
    juce::ComboBox styleSelector;
    GetParameterValue getValue;
    SetParameterValue setValue;
    std::function<void (int)> onStyleChanged;

    static constexpr int styleSelectorHeight = 24;
    static constexpr int topGap = 10;
    static constexpr int nameLabelHeight = 14;
    static constexpr int labelGap = 3;
    static constexpr int dialDiameter = 46;
    static constexpr int indicatorHeight = 14;
    static constexpr int columnWidth = 92;
    static constexpr int columnGap = 10;
    static constexpr int leftPadding = 4;

    // Vertical-drag rotary state -- -1 when no dial is being dragged.
    // Dragging is relative to the Y position/value at mouseDown (not the
    // dial's own bounds), the standard "drag up increases, drag down
    // decreases, sensitivity independent of where in the dial you grabbed"
    // rotary-knob convention.
    int draggingParamIndex = -1;
    int draggingStartY = 0;
    float draggingStartValue = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PlaybackStyleParameterPanel)
};
