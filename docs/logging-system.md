# NeditVST Logging & Crash Diagnostics System

## Purpose

A self-contained diagnostic logging system for NeditVST, designed for
debugging issues that only manifest inside a DAW host process (where
traditional debugging tools are unavailable or impractical).  This
document contains the complete implementation so it can be recreated in
future bug hunts.

## Architecture

```
  Message Thread                         Audio Thread
  ──────────────                         ────────────
  neditLog("createEditor")               processBlock lock contention
       │                                      │
       ▼                                      ▼
  ┌─────────────────────────────────────────────────┐
  │  logMutex (std::mutex)                          │
  │  → protects logFile (std::ofstream, append)     │
  │  → every write is timestamped [epoch_ms]        │
  └─────────────────────────────────────────────────┘
       │
       ▼
  ~/nedit_crash.log

  ──── On SIGSEGV / SIGABRT / SIGFPE ────

  neditLogSignalHandler()
       │
       ├── write() to ~/nedit_crash.log  (async-signal-safe)
       ├── write() to STDERR_FILENO      (Bitwig engine log)
       ├── backtrace_symbols_fd()        (both destinations)
       └── _exit(1)
```

## Log File Format

    [1784836244563] --- plugin loaded ---
    [1784836244577] Processor ctor
    [1784836244629] loadSample: /path/to/file.wav
    [1784836244663] loadSample: read 15089700 samples, 2 ch, 115.1 MB
    [1784836244700] createEditor
    Editor ctor: begin
    Editor ctor: done
    Editor dtor: begin
    [1784836254544] createEditor
    Editor ctor: begin
    Editor ctor: done
    *** SIGNAL / CRASH: SIGSEGV ***
    NeditVST.so(+0x13f432)[0x7dca7cf3f432]
    NeditVST.so(+0x408a06)[0x7dca7d208a06]
    ...

## Dependencies

| Header             | Purpose                              | Async-signal-safe |
|--------------------|--------------------------------------|--------------------|
| `<csignal>`        | signal() registration                | Yes                |
| `<cstring>`        | strlen() in signal handler           | Yes                |
| `<execinfo.h>`     | backtrace(), backtrace_symbols_fd()  | Yes                |
| `<fcntl.h>`        | open() in signal handler             | Yes                |
| `<unistd.h>`       | write(), close()                     | Yes                |
| `<fstream>`        | std::ofstream for log file           | No                 |
| `<mutex>`          | std::mutex for thread safety         | No                 |

---

## Implementation

### Step 1: PluginProcessor.cpp includes

Add these headers to the top of `Source/PluginProcessor.cpp`, after the
existing includes:

```cpp
#include <csignal>
#include <cstring>
#include <execinfo.h>
#include <fcntl.h>
#include <fstream>
#include <limits>
#include <mutex>
#include <unistd.h>
```

### Step 2: PluginProcessor.cpp logging namespace

Insert this anonymous namespace immediately after the includes (before
the existing `namespace { ... }` block that defines the juce wrapper):

```cpp
namespace
{
    std::ofstream logFile;
    std::mutex logMutex;

    void neditLog (const char* msg)
    {
        const std::lock_guard<std::mutex> lock (logMutex);
        if (! logFile.is_open())
        {
            auto path = juce::File::getSpecialLocation (juce::File::userHomeDirectory)
                            .getChildFile ("nedit_crash.log");
            logFile.open (path.getFullPathName().toRawUTF8(), std::ios::app);
        }
        // timestamp
        auto now = std::chrono::system_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds> (now.time_since_epoch()).count();
        logFile << "[" << ms << "] " << msg << "\n";
        logFile.flush();
    }

    void neditLogSignalHandler (int sig)
    {
        const char* sigName = "UNKNOWN";
        if (sig == SIGSEGV) sigName = "SIGSEGV";
        else if (sig == SIGABRT) sigName = "SIGABRT";
        else if (sig == SIGFPE) sigName = "SIGFPE";

        // Use only async-signal-safe functions (write, open, close).
        // Also write to the crash log file so the backtrace is easy to find.
        const char logPath[] = "/home/olly/nedit_crash.log";
        int logFd = open (logPath, O_WRONLY | O_CREAT | O_APPEND, 0644);

        auto safeWrite = [] (int fd, const char* s, size_t len)
        {
            size_t total = 0;
            while (total < len)
            {
                ssize_t n = write (fd, s + total, len - total);
                if (n <= 0) break;
                total += (size_t) n;
            }
        };

        const char header[] = "*** SIGNAL / CRASH: ";
        const char trailer[] = " ***\n";

        if (logFd >= 0)
        {
            safeWrite (logFd, header, sizeof(header) - 1);
            safeWrite (logFd, sigName, strlen(sigName));
            safeWrite (logFd, trailer, sizeof(trailer) - 1);
        }

        // Also write to stderr for Bitwig's engine log
        safeWrite (STDERR_FILENO, header, sizeof(header) - 1);
        safeWrite (STDERR_FILENO, sigName, strlen(sigName));
        safeWrite (STDERR_FILENO, trailer, sizeof(trailer) - 1);

        void* callstack[64];
        int frames = backtrace (callstack, 64);

        // Write backtrace to both stderr and the log file
        backtrace_symbols_fd (callstack, frames, STDERR_FILENO);
        if (logFd >= 0)
        {
            backtrace_symbols_fd (callstack, frames, logFd);
            safeWrite (logFd, "\n", 1);
            close (logFd);
        }

        ::_exit (1);
    }

    struct NeditLogInit { NeditLogInit()
    {
        neditLog ("--- plugin loaded ---");
        ::signal (SIGSEGV, neditLogSignalHandler);
        ::signal (SIGABRT, neditLogSignalHandler);
        ::signal (SIGFPE,  neditLogSignalHandler);
    } } neditLogInit;
}
```

### Step 3: PluginProcessor.cpp lifecycle logs

Add `neditLog(...)` calls at these locations:

**Constructor** — first line of `SlicerAudioProcessor::SlicerAudioProcessor()`:

```cpp
SlicerAudioProcessor::SlicerAudioProcessor()
    : AudioProcessor (BusesProperties()
                           .withOutput ("Output", juce::AudioChannelSet::stereo(), true))
{
    neditLog("Processor ctor");
    // ... rest of constructor unchanged
```

**Destructor** — replace `= default`:

```cpp
// was:  SlicerAudioProcessor::~SlicerAudioProcessor() = default;
SlicerAudioProcessor::~SlicerAudioProcessor() { neditLog("Processor dtor"); }
```

**createEditor:**

```cpp
juce::AudioProcessorEditor* SlicerAudioProcessor::createEditor()
{
    neditLog("createEditor");
    return new SlicerAudioProcessorEditor (*this);
}
```

**getStateInformation** — first line of the function body:

```cpp
void SlicerAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    neditLog("getStateInformation");
    // ... rest unchanged
```

**setStateInformation** — at entry, before the early-return, and at
completion:

```cpp
void SlicerAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    neditLog("setStateInformation: begin");
    // ... (existing xml parsing code) ...

    const juce::File sampleFile (tree.getProperty ("samplePath", ""));

    // Early-return when the same file is already loaded (avoids
    // re-reading a large buffer from disk on every DAW state restore):
    if (sampleLoaded && sampleFile == loadedFile)
    {
        neditLog("setStateInformation: same file, early return");
        if (onStateRestored)
            onStateRestored();
        neditLog("setStateInformation: early return done");
        return;
    }

    if (sampleFile.existsAsFile())
        loadSample (sampleFile);

    // ... (existing manual/excluded points restore, sliceProb restore) ...

    if (onStateRestored)
        onStateRestored();
    neditLog("setStateInformation: done");
}
```

**loadSample** — at entry and after reading:

```cpp
void SlicerAudioProcessor::loadSample (const juce::File& file)
{
    neditLog(("loadSample: " + file.getFullPathName()).toRawUTF8());
    // ... (existing reader creation, buffer allocation) ...

    reader->read (&newBuffer, 0, (int) reader->lengthInSamples, 0, true, true);
    {
        char buf[256];
        snprintf (buf, sizeof buf, "loadSample: read %d samples, %d ch, %.1f MB",
                  (int) reader->lengthInSamples, (int) reader->numChannels,
                  (float) reader->lengthInSamples * reader->numChannels * 4.0f / (1024.0f * 1024.0f));
        neditLog (buf);
    }

    // ... rest of loadSample unchanged ...
```

### Step 4: PluginProcessor.cpp lock contention monitor

Insert this block inside `processBlock()`, after `buffer.clear()` and
before the `sampleLock` acquisition:

```cpp
void SlicerAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;
    midiMessages.clear();
    buffer.clear();

    // Log if the audio thread had to wait more than 1ms for sampleLock
    // (i.e., the message thread was holding it too long).
    static int processBlockCount = 0;
    if (++processBlockCount % 500 == 0) // every ~11ms at 44.1kHz/512
    {
        const auto before = juce::Time::getHighResolutionTicks();
        const bool got = sampleLock.tryEnter();
        const auto after = juce::Time::getHighResolutionTicks();
        const double waitMs = (double) (after - before) * 1000.0 / (double) juce::Time::getHighResolutionTicksPerSecond();

        if (got)
        {
            sampleLock.exit();
            if (waitMs > 1.0)
            {
                char buf[128];
                snprintf (buf, sizeof buf, "processBlock: lock wait %.2f ms", waitMs);
                neditLog (buf);
            }
        }
        else
        {
            neditLog ("processBlock: tryEnter FAILED");
        }
    }

    const juce::ScopedLock sl (sampleLock);
    // ... rest of processBlock unchanged ...
```

### Step 5: PluginEditor.cpp editor-side logging

Add a separate `neditLog` in `Source/PluginEditor.cpp` (the editor has
its own — no mutex, no timestamps — for ctor/dtor messages that fire
before the processor's ofstream is guaranteed open):

**At the top of the file**, after the includes:

```cpp
#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
    void neditLog (const char* msg)
    {
        auto path = juce::File::getSpecialLocation (juce::File::userHomeDirectory)
                        .getChildFile ("nedit_crash.log");
        juce::String line = juce::String (msg) + "\n";
        path.appendText (line);
    }
}
```

**In the constructor** — first and last lines:

```cpp
SlicerAudioProcessorEditor::SlicerAudioProcessorEditor (SlicerAudioProcessor& p)
    : AudioProcessorEditor (&p), processor (p), waveformDisplay (p), subdivisionGrid (p), playbackStyleGrid (p)
{
    neditLog("Editor ctor: begin");
    // ... (entire constructor body unchanged) ...
    setSize (900, 780);
    neditLog("Editor ctor: done");
}
```

**In the destructor** — first line:

```cpp
SlicerAudioProcessorEditor::~SlicerAudioProcessorEditor()
{
    neditLog("Editor dtor: begin");
    // ... (rest unchanged) ...
}
```

### Step 6: WaveformDisplay.cpp rebuild timing

Add wall-clock measurement around the peak-building loop in
`WaveformDisplay::rebuildWaveformPeaks()`:

```cpp
void WaveformDisplay::rebuildWaveformPeaks()
{
    waveformPeaks.clear();

    if (! processor.hasSample())
        return;

    const auto& buffer = processor.getSampleBuffer();
    const int numSamples = buffer.getNumSamples();
    const int numChannels = buffer.getNumChannels();
    const int width = juce::jmax (1, getWidth());

    if (numSamples == 0 || numChannels == 0)
        return;

    auto t0 = juce::Time::getHighResolutionTicks();

    waveformPeaks.resize ((size_t) width);

    // ... (existing peak-building loop unchanged) ...

    auto t1 = juce::Time::getHighResolutionTicks();
    double ms = (double) (t1 - t0) * 1000.0 / (double) juce::Time::getHighResolutionTicksPerSecond();
    if (ms > 5.0)
    {
        char buf[128];
        snprintf (buf, sizeof buf, "rebuildWaveformPeaks: %.1f ms (%d samples, %d ch, %d px)", ms, numSamples, numChannels, width);
        fprintf (stderr, "%s\n", buf);
    }
}
```

---

## Design Decisions

1. **Separate file, not JUCE Logger.**  JUCE's Logger goes to stdout
   which may be swallowed by the host.  A dedicated file under the
   user's home directory is always accessible.

2. **Two neditLog implementations.**  The editor has its own
   file-append implementation (no mutex, no timestamps) for
   construction/destruction messages that must be logged before the
   processor's `std::ofstream` is guaranteed to be open.

3. **Async-signal-safe crash handler.**  The signal handler avoids all
   non-signal-safe functions (no `malloc`, no `mutex`, no C++ iostream).
   It uses raw POSIX `open`/`write`/`close` and
   `backtrace_symbols_fd` (which writes directly to a file descriptor).

4. **Lazy file open.**  The log file is opened on first `neditLog` call,
   not at static initialisation, to avoid issues with file system
   availability during early plugin loading.

5. **Periodic, not per-block, lock monitoring.**  Checking contention on
   every `processBlock` call would add cost to the audio thread.
   Sampling every 500th call (~11 ms) is sufficient to detect
   contention while keeping overhead negligible.

## Usage During Bug Hunts

1. Apply all steps above to the current codebase.
2. Build and install the plugin.
3. Reproduce the issue.
4. Check `~/nedit_crash.log` for:
   - **Lifecycle timeline** — ctor/dtor/loadSample/createEditor
     timestamps show what the plugin was doing when things went wrong.
   - **Lock contention** — "processBlock: lock wait X ms" or
     "tryEnter FAILED" indicates the message thread and audio thread
     are fighting over sampleLock.
   - **Crash backtrace** — the exact call stack at the point of failure,
     with function names and offsets for `addr2line` resolution.
5. For symbol resolution: `addr2line -e NeditVST.so -f -C 0xOFFSET`
