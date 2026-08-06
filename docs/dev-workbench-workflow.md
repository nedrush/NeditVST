# Dev Workbench: Branch Strategy & Profiling Workflow

## Purpose

`main` is owned by the musician (nedrush) and should stay untouched by
our profiling/optimization/testing churn.  All of that work lives on a
long-lived integration branch, `dev/optimize-tests`, which tracks `main`
via rebase.  `main` only ever advances through a PR we open from that
branch, so the musician can review (or ignore) the collected fixes as a
single clean batch.

## Branch Layout

    main                <- musician's work; we never commit here directly
      |
      +-- dev/optimize-tests   <- our workbench; long-lived, rebased onto main

Everything we do -- profiling notes, perf fixes, test suites, CI wiring --
is committed on `dev/optimize-tests`.  Keep each logical change in its own
commit so the eventual PR is reviewable.

## When main Updates

    git fetch origin
    git rebase origin/main
    git push --force-with-lease origin dev/optimize-tests

Rebase early and often so conflicts stay small.  Our changes are mostly
confined to a few files (`Source/WaveformDisplay.cpp`,
`Source/PluginProcessor.cpp`, plus new test/CI files), so conflicts with
the musician's feature work are rare and mechanical.  Rebase rewrites
history, hence the force-push (with lease).

## Collecting a Batch of Fixes

    git checkout main
    git pull
    git merge dev/optimize-tests
    git push origin main
    # open a PR from origin/dev/optimize-tests to main

Open the PR (rather than merging locally) so the musician has the option
of stepping in.  After the merge, the workbench branch can stay around for
the next cycle.

## Profiling Cheat Sheet

The plugin's UI/timer/paint code runs inside the host's audio-engine
process, NOT the main GUI process.  For Bitwig:

    ENGINE=$(pgrep -f BitwigAudioEngine | head -1)
    grep NeditVST /proc/$ENGINE/maps      # sanity check: must list the plugin

Capture (needs sudo; output files are root-owned -- chmod them so the
analysis tools can read them):

    sudo perf record -F 499 -g -p $ENGINE -o ndtfixedX.perf -- sleep 10
    sudo chmod 644 ndtfixedX.perf

Analyze:

    perf report -i ndtfixedX.perf --no-children
    perf script -i ndtfixedX.perf | grep -c NeditVST   # > 0 or the capture is useless

Release builds lose frame-pointer callgraph accuracy, so `perf` stack
traces above the leaf symbol are often garbage -- use the symbol /
temporal-spread analysis instead of trusting `--call-graph`.

## Known Findings (as of Aug 2026)

- Redraw bug #1 (SIGSEGV on editor reopen): fixed, see
  `docs/bugfix-editor-reopen-sigsegv.md`.
- Redraw bug #2: `WaveformDisplay::paint()` re-ran
  `TransientDetector::detectSlices()` (full-buffer, 2 passes) on every
  repaint.  Fixed by `c807c76` (cached marker sets in
  `rebuildSlicesFromDetectionAndManualPoints()`).
- Remaining idle cost (validated on the fixed branch):
  - The 30 Hz `WaveformDisplay::timerCallback()` repaints unconditionally;
    JUCE's software rasterizer re-draws ~900 vertical lines per frame.
  - `rebuildWaveformPeaks()` linearly scans the whole visible range on
    every zoom/refresh/resize -- O(visible samples) per call.
  - Detection no longer runs at idle (fix confirmed working).

## Housekeeping

- `*.perf` / `*.perf.old` are gitignored (profile captures are large and
  machine-local).
- Untracked captures can stay in the repo root; they won't pollute
  `git status`.
