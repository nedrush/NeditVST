#pragma once

#include <JuceHeader.h>
#include <cstdlib>
#include <cstring>

//==============================================================================
// Minimal message-thread profiling instrumentation for NeditVST.
//
// JUCE 8.x removed the old juce::Profiler / PerformanceCounter classes, so
// this is a tiny self-contained replacement: RAII scoped sections that
// accumulate call counts and wall-clock time, dumped once per second (from
// the editor's existing 10 Hz timer) to stderr -- which lands in the DAW's
// engine log.
//
// Enabled via the NEDITVST_PROFILE=1 environment variable (set it before
// launching the DAW). When disabled, sections still count calls (so we can
// see how often e.g. rebuildWaveformPeaks runs) but skip all timing,
// keeping overhead effectively zero.
//
// All access happens on the message thread (paint / timerCallback / refresh
// / detection are all message-thread), so no locking is needed.
//==============================================================================

namespace Perf
{
    constexpr int maxSections = 16;

    struct Section
    {
        const char* name = nullptr;
        juce::uint64 count = 0;
        juce::uint64 totalTicks = 0;
        juce::uint64 maxTicks = 0;
    };

    struct Table
    {
        Section sections[maxSections];
        int numSections = 0;
    };

    inline Table& table() noexcept
    {
        static Table t;
        return t;
    }

    inline Section& section (const char* name) noexcept
    {
        auto& t = table();

        for (int i = 0; i < t.numSections; ++i)
            if (t.sections[i].name == name)
                return t.sections[i];

        if (t.numSections < maxSections)
        {
            t.sections[t.numSections].name = name;
            return t.sections[t.numSections++];
        }

        static Section overflow;
        return overflow;
    }

    inline bool isEnabled() noexcept
    {
        static const bool enabled = [] {
            const char* env = std::getenv ("NEDITVST_PROFILE");
            return env != nullptr && std::strcmp (env, "1") == 0;
        }();
        return enabled;
    }

    inline void reset() noexcept
    {
        auto& t = table();
        for (int i = 0; i < t.numSections; ++i)
        {
            t.sections[i].count = 0;
            t.sections[i].totalTicks = 0;
            t.sections[i].maxTicks = 0;
        }
    }

    // Scoped timing of one named section. Counts the call regardless of
    // whether timing is enabled, so call-frequency is observable even with
    // NEDITVST_PROFILE unset.
    struct ScopedSection
    {
        const char* name;
        juce::uint64 startTicks;

        explicit ScopedSection (const char* n) noexcept : name (n)
        {
            startTicks = isEnabled() ? juce::Time::getHighResolutionTicks() : 0;
        }

        ~ScopedSection() noexcept
        {
            auto& s = section (name);
            ++s.count;

            if (isEnabled())
            {
                const auto ticks = juce::Time::getHighResolutionTicks() - startTicks;
                s.totalTicks += ticks;
                if (ticks > s.maxTicks)
                    s.maxTicks = ticks;
            }
        }
    };

    inline void dump (juce::OutputStream& out, juce::uint64 wallTicks) noexcept
    {
        const double ticksPerMs = (double) juce::Time::getHighResolutionTicksPerSecond() / 1000.0;

        out << "NeditVST profile over " << (double) wallTicks / ticksPerMs << " ms:\n";

        auto& t = table();
        for (int i = 0; i < t.numSections; ++i)
        {
            const auto& s = t.sections[i];
            if (s.count == 0)
                continue;

            const double avgMs = (double) s.totalTicks / (double) s.count / ticksPerMs;
            const double maxMs = (double) s.maxTicks / ticksPerMs;

            out << "  " << s.name << ": " << (juce::int64) s.count << " calls, avg "
                << avgMs << " ms, max " << maxMs << " ms\n";
        }
    }

    inline void dumpToStderr (juce::uint64 wallTicks) noexcept
    {
        juce::MemoryOutputStream mo;
        dump (mo, wallTicks);
        mo << '\n';
        fprintf (stderr, "%s", mo.toString().toRawUTF8());
    }
}
