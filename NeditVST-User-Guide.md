# NeditVST — User Guide (First Draft)

A generative, transient-based loop slicer. Load a drum loop or break, let
NeditVST detect the hits, and it'll play back an evolving, weighted-random
arrangement of those slices in sync with your DAW's tempo — or program it
by hand with the built-in step sequencer.

This is early-stage software being shared with a small group of testers.
Expect rough edges. Feedback on anything confusing, broken, or just not
fun is genuinely welcome.

---

## 1. Getting started

- **Load Sample...** button, or drag an audio file straight onto the
  waveform display.
- Once loaded, you'll see the waveform with white vertical lines marking
  detected transients — these are your slices.
- Press play in your DAW. NeditVST won't make sound until the host
  transport is running — it has no "preview" playback of its own outside
  of the Audition button (see below).

## 2. Trimming and tempo sync

Most loops aren't perfectly cropped — there's often a bit of silence at
the start or end, or the file isn't an exact number of bars. NeditVST
needs to know exactly which part of the file is "the loop" to calculate
tempo correctly.

- **Trim Start / Trim End** — drag these markers on the waveform to bound
  just the musical content. They snap to nearby transients automatically;
  hold **Shift** while dragging to place them freely without snapping.
- **Audition** button — plays back just the trimmed region on a loop, at
  its original pitch and speed, completely bypassing the generative
  engine. Use this to count bars by ear before setting the next control.
- **Loop length (bars)** — tell NeditVST how many bars the trimmed region
  actually is. This is what lets it calculate the loop's original tempo
  and sync it correctly to your DAW.
- **Manual BPM override** — if you already know the loop's real tempo,
  type it directly instead of relying on the bar-count calculation.

**If you move a trim handle, the Loop Length field will highlight in
orange** until you interact with it again — this is a reminder to
double-check the bar count still matches, since a stale value there is
the single most common cause of things sounding out of time.

## 3. Transient detection

- **Transient sensitivity** — how aggressively NeditVST looks for hits in
  the waveform. Drag it and watch the slice markers update live.
- **Quantize Transients** + **Grid** — optionally snaps auto-detected
  transients onto a musical grid (choose the resolution), correcting for
  natural imprecision in the detected hit positions. Doesn't affect
  manually-placed slice points.
- **Manual slice points** — double-click anywhere on the waveform to add
  your own slice point (snaps to the nearest real transient). Drag an
  existing one to move it. Hold **Shift** while adding or dragging to
  bypass snapping entirely. **Cmd/Ctrl-click** (or double-click) a slice
  point to delete it — works on both manual points (magenta) and
  auto-detected ones (white).
- **Reset edits** button — instantly clears every manual addition and
  deletion, back to pure auto-detection at the current sensitivity. Your
  safety net if you've made a mess.
- **Undo / Redo** — covers every slice-editing action (add, move, remove,
  delete, reset) as individual steps.

## 4. Pitch Mode

Controls how NeditVST handles pitch when your DAW's tempo doesn't match
the loop's original tempo.

- **Repitch** — classic varispeed. Speed and pitch are linked (like
  changing a turntable's speed) — faster tempo means higher pitch.
- **Time-Stretch** — a granular engine keeps pitch independent of tempo.
  Adjustable **grain size**, **window shape**, and a **pitch shift**
  control (semitones) that's independent of tempo entirely.
- **Beat-quantize slice length** — available in both modes (default ON
  for Time-Stretch, OFF for Repitch) — forces each slice's *rendered*
  length onto the nearest musical subdivision, which helps timing
  accuracy on loosely-cropped or imperfectly-detected material. In
  Repitch mode this introduces a small amount of per-hit pitch variance
  as a side effect; in Time-Stretch mode it's essentially free.

## 5. Trigger Mode

Controls *when* slices get picked and played.

- **Slice Length** — the classic generative mode. Each pick plays out its
  own full natural length before the next weighted-random pick happens.
  Free-running, not locked to a fixed grid.
  - **Reset every [1/2/4/8] bars** — periodically forces a fresh pick
    exactly on the bar line, keeping the free-running chain from drifting
    too far out of phase with your DAW over long stretches.
- **Clock** — a fixed clock window (choose the reference note value) picks
  one slice and one subdivision rate together at the top of each window,
  then retriggers that slice at the subdivision rate for the rest of the
  window — a rhythmic "roll" feel, tightly locked to the grid.
- **Sequenced** — fully manual, hand-programmed step sequencer. See
  section 6.

## 6. Playback Styles

Each time a slice is picked, it can play back in one of several ways —
weighted by probability (in Slice Length/Clock modes) or hand-assigned
per step (in Sequenced mode):

- **Forward** — normal playback.
- **Ping-Pong** — plays forward, then reverses and plays backward.
- **Tape Stop** — decelerates to a stop, like letting go of a spinning
  record.
- **Stretch** — extreme, deliberately mangled granular timestretch —
  glitchy, pitched, characterful (think classic jungle/breakbeat
  timestretch artifacts).
- **Filter Down / Filter Up** — a resonant filter sweep, closing or
  opening over the duration of the hit.
- **Scratch** — rapid forward/backward cycling with a real pitch-bend
  curve, closer to a turntable scratch than Ping-Pong's constant-speed
  bounce. Rate, plus independent curve shapes for the forward and
  backward strokes.
- **Bitcrush** — lo-fi digital degradation — sample-rate reduction and
  bit-depth reduction, each independently adjustable.
- **Flanger** — a short modulated delay mixed with the dry signal for
  a classic sweeping comb-filter "whoosh." Delay time, mix, and
  feedback, each independently adjustable.

Every style with adjustable parameters (Scratch, Bitcrush, Flanger,
Filter, Stretch) can also have those parameters **swept** across the
duration of the hit rather than held at one fixed value — usually a
choice between Static, Sweep In, and Sweep Out (or, for Volume
specifically, Ramp Up/Ramp Down — see the Sequencer section).

**Two ways to adjust these parameters:**
- **Slice Length / Clock mode** — a persistent panel: pick a style
  from the selector, its parameters appear below, ready to edit
  directly. Changing a value here changes the shared default used
  everywhere that style is picked without its own per-step override.
- **Sequencer mode** — right-click an individual step for that one
  step's own settings (see below) — these override the shared default
  just for that step.

## 7. The Sequencer

Switch **Trigger Mode** to **Sequenced** to reveal the step grid.

- Each **row** is one available slice (bottom row = earliest detected
  transient, working up); each **column** is a time step. Row height
  scales to fit the available space — a sample with only a few slices
  gets taller, easier-to-see rows; a busy sample with many detected
  slices falls back to thinner rows with scrolling.
- **Click and drag** to draw steps in. A step's length automatically
  reflects that slice's real duration — a 3-eighth-note-long slice draws
  in as a 3-eighth-note-long bar, not a single generic block.
- Only one step can be active per column across the *entire* grid at
  once — drawing a new one clears any conflicting step elsewhere in that
  column.
- **Pattern length (bars)** and **step resolution** control the grid's
  overall size.
- **Playback Style palette** — pick a color-coded style, then draw with
  it — the step takes on that color and plays back with that style.
- **Randomize Sequence** — generates a pattern automatically, respecting
  each slice's natural length and drawing styles from the same weighted
  probabilities used elsewhere. Each style in the palette has its own
  small checkbox — check a style's box and any step Randomize assigns
  that style to will also get fully randomized parameters (values *and*
  Static/Sweep modes); left unchecked (the default), that style keeps
  its plain default settings when randomized.
- **Clear** — wipes the pattern.
- **Right-click an active step** (click near the left edge of the step)
  to open its parameter menu — adjust that specific step's resonance,
  filter type, curve shape, grain settings, rate, delay/mix/feedback,
  **Volume** (Static / Ramp Up / Ramp Down — raises or lowers the
  step's volume smoothly across its own duration), or **Subdivide**
  (retriggers that one step internally at a chosen rate — its own mini
  roll). Drag the slider that appears, release to confirm — a small
  triangle marks any step with a customized value. Volume ramps (and
  effect sweeps) correctly glide across the *entire* step even when
  Subdivide is also retriggering underneath them.
- **Shift+drag a step's right edge to extend it** beyond its slice's
  natural length. What happens with that extra time depends on the
  step's playback style:
  - **Forward / Ping-Pong** — loop to fill the extra time (Forward
    repeats the slice from the top; Ping-Pong repeats additional
    forward/backward round trips).
  - **Tape Stop / Stretch / Filter Down / Filter Up** — the effect
    itself stretches to span the *entire* extended length: Tape Stop's
    decel takes the full extended time to reach silence, Stretch's
    mangle runs for the whole duration (looping its output if it
    finishes early), and a Filter sweep glides continuously across the
    whole extended step as one motion — even if Subdivide is also
    retriggering the slice underneath it.

## 8. Performance Mode

Switch **Trigger Mode** to **Performance** for a completely different way
of working — instead of one continuous generative/sequenced engine,
you define several hand-picked **states** (a segment of the loaded
sample plus a playback style) and recall them live from a MIDI
keyboard, like flipping between presets.

**Important:** you'll need a MIDI keyboard or controller connected and
routed into NeditVST in your DAW for this mode to do anything — it's
entirely MIDI-driven.

- **On-screen keyboard UI** — click a key to give it **focus** for
  editing. Keys with a saved state show a highlight; the focused key
  shows a different highlight. Clicking a key doesn't play anything by
  itself — it's purely for selecting what you're editing.
- **While a key has focus**, the trim handles, Playback Style selector,
  and parameter panel all edit *that* state. Everything here works
  **independent of your DAW's transport** — you can shape a sound with
  playback stopped, no need to press play first.
- **Per-state Loop toggle** — on: the state plays continuously once
  recalled, until a different state is triggered. Off: plays once
  through the segment, then stops (a one-shot).
- **Per-state Sync toggle** — on: uses whichever global Pitch Mode
  (Repitch/Time-Stretch) is active, synced to your DAW's tempo. Off:
  plays at the segment's native pitch and speed, completely unsynced —
  useful for material that isn't meant to lock to a grid.
- **Trim snapping mode** — **Transients** (snaps to detected hits, the
  default everywhere else) or **Grid** (snaps to a fixed musical grid
  instead, using the loop's already-established tempo — better for
  chopping out an exact musical length like "one 16th note" rather than
  wherever a transient happens to sit). Hold **Shift** to bypass
  snapping entirely, either mode.
- **Auto-save** — there's no separate save button. The moment you click
  a *different* key on the on-screen keyboard to move focus elsewhere,
  whatever you were just editing is saved automatically.
- **Playing states from your physical MIDI keyboard:**
  - Pressing the **currently focused** key's physical note **auditions
    your in-progress edits** live — hear changes as you make them.
  - Pressing **any other** key plays whatever's already **saved**
    there, without changing focus — you can freely check other states
    while still actively editing a different one.
- **Quantize Recall** — off by default (recall is instant). Turn it on
  and pick a note value, and physical key presses wait for the next
  occurrence of that musical grid point before switching, rather than
  switching the instant you press — useful for keeping live state
  changes landing on the beat. Falls back to instant switching if your
  DAW's transport isn't running, since there's no beat to quantize
  against without it.

## 9. Known limitations (this build)

- Best results come from well-prepared source material — clean drum
  loops and breaks. Non-rhythmic material (vocals, arbitrary sound
  collages) isn't what the tempo-sync engine is designed for yet.
- VST3 only — no AU.
- Builds are unsigned (see troubleshooting below if it doesn't show up
  in your DAW).
- Sequencer currently supports up to 32 rows (slices) and is
  single-voice (one thing plays at a time, even across rows).
- The **Control** sub-mode (sequencing from your DAW's piano roll)
  isn't built yet — Performance mode above is the only MIDI-driven way
  to work with NeditVST right now.

## 10. Troubleshooting: plugin doesn't show up (Mac)

This build isn't code-signed yet, so macOS often blocks it silently
rather than showing a clear warning — the plugin just won't appear in
your DAW's plugin list at all, with no obvious explanation. If that's
what's happening:

1. **Check the file actually made it to the right place.** The download
   comes as a `.zip` — make sure you've actually unzipped it, and that
   the resulting `NeditVST.vst3` (not the zip itself, and not an extra
   folder from unzipping) is sitting directly inside:
   `~/Library/Audio/Plug-Ins/VST3/`
2. **Clear the quarantine flag.** Open **Terminal** (Spotlight search
   "Terminal") and paste this exactly, then press Return:
   ```
   xattr -cr ~/Library/Audio/Plug-Ins/VST3/NeditVST.vst3
   ```
3. **Fully quit and reopen your DAW** — not just a plugin rescan, an
   actual quit and relaunch.
4. Still not showing? Trigger a manual plugin rescan in your DAW's
   preferences (in Ableton Live: Preferences → Plug-Ins).

If it *does* show a warning dialog when you first try to load it rather
than just not appearing, that's Gatekeeper — just confirm you trust it
and proceed; that's expected for an unsigned build and isn't harmful.

## 11. Building from source

If you'd rather build it yourself than trust a downloaded binary — or if
the fixes above didn't work — the repo builds the same way on all three
platforms. **Building locally on Mac also sidesteps the Gatekeeper issue
entirely**, since a plugin built on your own machine is automatically
trusted by that same machine.

**You'll need first:** [CMake](https://cmake.org/download) (3.22+) and a
C++ compiler for your platform (see below) — Git is optional (see below).
The first build downloads and compiles JUCE itself from scratch, so
expect it to take 10–20 minutes — later builds are much faster.

### Getting the source code

Two ways to do this — pick whichever's more comfortable:

- **Easiest, no extra software needed:** go to
  [github.com/nedrush/NeditVST](https://github.com/nedrush/NeditVST) in
  your browser, click the green **"Code"** button, then **"Download
  ZIP"**. Unzip it — that unzipped folder is what you'll `cd` into below,
  instead of running the `git clone` line.
- **If you're comfortable with Git:** the `git clone` command in each
  section below does the same thing from the command line, and makes it
  easy to grab updates later without re-downloading everything.

### Mac

```
xcode-select --install          # if you don't already have Xcode's command-line tools

git clone https://github.com/nedrush/NeditVST.git
cd NeditVST
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

The built plugin lands at `build/NeditVST_artefacts/Release/VST3/NeditVST.vst3`
— copy that into `~/Library/Audio/Plug-Ins/VST3/`.

### Windows

Install **Visual Studio Community** (free) with the **"Desktop
development with C++"** workload selected during setup, plus CMake.
Git is only needed if you're using `git clone` above instead of
downloading the ZIP.

```
git clone https://github.com/nedrush/NeditVST.git
cd NeditVST
cmake -B build
cmake --build build --config Release
```

Copy the resulting `NeditVST.vst3` from `build\NeditVST_artefacts\Release\VST3\`
into `C:\Program Files\Common Files\VST3\`.

### Linux

```
sudo apt-get update
sudo apt-get install -y build-essential cmake git \
    libasound2-dev libjack-dev ladspa-sdk libcurl4-openssl-dev \
    libfreetype-dev libx11-dev libxcomposite-dev libxcursor-dev \
    libxext-dev libxinerama-dev libxrandr-dev libxrender-dev \
    libwebkit2gtk-4.1-dev libglu1-mesa-dev mesa-common-dev

git clone https://github.com/nedrush/NeditVST.git
cd NeditVST
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

Copy the built `NeditVST.vst3` into `~/.vst3/`.

---

Thanks for testing. If something's unclear, confusing, or just doesn't
sound right, that's exactly the kind of feedback that's most useful right
now.
