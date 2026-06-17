#pragma once

// =============================================================================
// In-process sampling profiler (header-only, include ONCE from main_launcher).
//
// The per-frame bucket telemetry (PERF_SPARSE_STEPS etc.) can't find the real
// CPU stutter: the cost lands in an undecomposed "untracked / postWait" gap and
// the periodic loggers miss the spike frames entirely. This profiler answers
// "what is the CPU actually doing?" directly, independent of any instrumentation:
//
//   A background thread suspends the MAIN thread every ~1ms, reads its
//   instruction pointer, resumes it, and symbolizes the address (needs the PDB
//   from the /Z7 + /DEBUG build). Two outputs:
//     * PROF_STALL  - logged LIVE the moment the main thread sits in one
//                     function longer than VENPOD_PROFILE_STALL_MS (default 30)
//                     -> names the function responsible for each freeze/spike.
//     * PROF_TOP    - at shutdown, the top self-time functions (where steady CPU
//                     goes) so the bottleneck is named, not guessed.
//
// Enable: VENPOD_PROFILE=1. Tunables: VENPOD_PROFILE_STALL_MS,
// VENPOD_PROFILE_INTERVAL_MS, VENPOD_PROFILE_TOP_N.
// =============================================================================

#include <windows.h>
#include <dbghelp.h>
#include <timeapi.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>

#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "winmm.lib")

namespace VENPOD::Tools {

class SamplingProfiler {
public:
    // Call ONCE on the main thread (it captures the calling thread as the target).
    void Start() {
        const char* enable = std::getenv("VENPOD_PROFILE");
        if (enable == nullptr || enable[0] == '0' || enable[0] == '\0') {
            return;
        }
        m_stallMs = ReadEnvU32("VENPOD_PROFILE_STALL_MS", 30u);
        m_hitchMs = ReadEnvU32("VENPOD_PROFILE_HITCH_MS", 40u);
        m_intervalMs = std::max(1u, ReadEnvU32("VENPOD_PROFILE_INTERVAL_MS", 1u));
        m_topN = std::max(1u, ReadEnvU32("VENPOD_PROFILE_TOP_N", 30u));
        m_captureStacks = ReadEnvU32("VENPOD_PROFILE_STACKS", 0u) != 0u;

        // Duplicate the main thread's (real) handle so the sampler can suspend it.
        if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(),
                             GetCurrentProcess(), &m_mainThread,
                             THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT,
                             FALSE, 0)) {
            spdlog::warn("PROF: DuplicateHandle failed ({}), profiler disabled", GetLastError());
            return;
        }
        SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME | SYMOPT_LOAD_LINES);
        if (!SymInitialize(GetCurrentProcess(), nullptr, TRUE)) {
            spdlog::warn("PROF: SymInitialize failed ({}), names may be unavailable", GetLastError());
        }
        m_enabled = true;
        m_running.store(true, std::memory_order_release);
        m_thread = std::thread([this]() { SampleLoop(); });
        spdlog::info("PROF: sampling profiler ON (interval={}ms stallThreshold={}ms)",
                     m_intervalMs, m_stallMs);
    }

    // Call ONCE at shutdown (any thread).
    void Stop() {
        if (!m_enabled) {
            return;
        }
        m_running.store(false, std::memory_order_release);
        if (m_thread.joinable()) {
            m_thread.join();
        }
        DumpTop();
        SymCleanup(GetCurrentProcess());
        if (m_mainThread) {
            CloseHandle(m_mainThread);
            m_mainThread = nullptr;
        }
        m_enabled = false;
    }

    ~SamplingProfiler() { Stop(); }

    // Call once per frame from the main loop so the sampler can attribute its samples
    // to frames and dump a per-function breakdown of any frame slower than the hitch
    // threshold (PROF_HITCH). Cheap (one relaxed atomic store); no-op if profiling off.
    void MarkFrame(uint32_t frameIndex) {
        m_currentFrame.store(frameIndex, std::memory_order_relaxed);
    }

private:
    static uint32_t ReadEnvU32(const char* name, uint32_t fallback) {
        const char* v = std::getenv(name);
        if (v == nullptr || v[0] == '\0') {
            return fallback;
        }
        char* end = nullptr;
        const unsigned long parsed = std::strtoul(v, &end, 10);
        return (end == v) ? fallback : static_cast<uint32_t>(parsed);
    }

    // Resolve an instruction pointer to "module!function". Cached by the symbol's
    // base address so SymFromAddr isn't called for every one of ~1000 samples/sec.
    const std::string& ResolveName(DWORD64 addr) {
        alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME];
        auto* sym = reinterpret_cast<SYMBOL_INFO*>(buffer);
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = MAX_SYM_NAME;
        DWORD64 disp = 0;
        if (SymFromAddr(GetCurrentProcess(), addr, &disp, sym)) {
            auto it = m_nameByBase.find(sym->Address);
            if (it != m_nameByBase.end()) {
                return it->second;
            }
            return m_nameByBase.emplace(sym->Address, std::string(sym->Name)).first->second;
        }
        // Unsymbolized (system dll / no PDB): bucket by 64KB region so the histogram
        // still groups, and the address is visible for manual lookup.
        const DWORD64 region = addr & ~static_cast<DWORD64>(0xFFFF);
        auto it = m_nameByBase.find(region);
        if (it != m_nameByBase.end()) {
            return it->second;
        }
        char hex[32];
        std::snprintf(hex, sizeof(hex), "0x%llx", static_cast<unsigned long long>(region));
        return m_nameByBase.emplace(region, std::string("<unresolved ") + hex + ">").first->second;
    }

    // One stack frame above the leaf (the caller's return address). The main thread is
    // suspended, so its stack is stable to walk. ctx is copied (StackWalk64 mutates it).
    DWORD64 WalkCaller(const CONTEXT& ctxIn) {
        CONTEXT ctx = ctxIn;
        STACKFRAME64 frame{};
        frame.AddrPC.Offset = ctx.Rip;
        frame.AddrPC.Mode = AddrModeFlat;
        frame.AddrFrame.Offset = ctx.Rbp;
        frame.AddrFrame.Mode = AddrModeFlat;
        frame.AddrStack.Offset = ctx.Rsp;
        frame.AddrStack.Mode = AddrModeFlat;
        for (int i = 0; i < 2; ++i) {
            if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, GetCurrentProcess(), m_mainThread,
                             &frame, &ctx, nullptr, SymFunctionTableAccess64,
                             SymGetModuleBase64, nullptr)) {
                return 0;
            }
            if (i == 1) {
                return frame.AddrPC.Offset;
            }
        }
        return 0;
    }

    void SampleLoop() {
        timeBeginPeriod(1);
        const HANDLE proc = GetCurrentProcess();
        (void)proc;
        std::string lastName;
        uint32_t stallSamples = 0;
        bool stallLogged = false;
        LARGE_INTEGER freq;
        QueryPerformanceFrequency(&freq);
        // Per-frame hitch capture: the main loop bumps m_currentFrame once per frame.
        // While a frame hitches (main thread stuck), m_currentFrame doesn't advance, so
        // this window keeps accumulating that frame's samples; when the frame finally
        // ends we dump the breakdown if it ran longer than the hitch threshold.
        uint32_t lastFrame = m_currentFrame.load(std::memory_order_relaxed);
        LARGE_INTEGER frameStart;
        QueryPerformanceCounter(&frameStart);
        std::unordered_map<std::string, uint32_t> frameHist;

        while (m_running.load(std::memory_order_acquire)) {
            DWORD64 rip = 0;
            DWORD64 callerRip = 0;
            if (SuspendThread(m_mainThread) != static_cast<DWORD>(-1)) {
                alignas(16) CONTEXT ctx{};
                ctx.ContextFlags = m_captureStacks ? CONTEXT_FULL : CONTEXT_CONTROL;
                if (GetThreadContext(m_mainThread, &ctx)) {
                    rip = ctx.Rip;
                    if (m_captureStacks) {
                        callerRip = WalkCaller(ctx);  // 1 frame up = the leaf's caller
                    }
                }
                ResumeThread(m_mainThread);
            }
            if (rip != 0) {
                const std::string& name = ResolveName(rip);
                ++m_samplesByName[name];
                ++m_totalSamples;
                if (m_captureStacks && callerRip != 0) {
                    ++m_callersByLeaf[name][ResolveName(callerRip)];
                }

                // Frame-window tracking for hitch capture.
                const uint32_t curFrame = m_currentFrame.load(std::memory_order_relaxed);
                if (curFrame != lastFrame) {
                    LARGE_INTEGER now;
                    QueryPerformanceCounter(&now);
                    const double frameMs = 1000.0 *
                        static_cast<double>(now.QuadPart - frameStart.QuadPart) /
                        static_cast<double>(freq.QuadPart);
                    if (frameMs >= static_cast<double>(m_hitchMs) && !frameHist.empty()) {
                        DumpHitchFrame(lastFrame, frameMs, frameHist);
                    }
                    frameHist.clear();
                    frameStart = now;
                    lastFrame = curFrame;
                }
                ++frameHist[name];

                // Stall watchdog: same function across consecutive samples.
                if (name == lastName) {
                    ++stallSamples;
                    const uint32_t elapsedMs = stallSamples * m_intervalMs;
                    if (!stallLogged && elapsedMs >= m_stallMs) {
                        spdlog::warn("PROF_STALL func={} held>={}ms (single function, main thread blocked here)",
                                     name, elapsedMs);
                        stallLogged = true;
                    }
                } else {
                    if (stallLogged) {
                        spdlog::warn("PROF_STALL_END func={} total~{}ms",
                                     lastName, stallSamples * m_intervalMs);
                    }
                    lastName = name;
                    stallSamples = 1;
                    stallLogged = false;
                }
            }
            Sleep(m_intervalMs);
        }
        timeEndPeriod(1);
    }

    void DumpTop() {
        if (m_totalSamples == 0) {
            spdlog::info("PROF_TOP: no samples collected");
            return;
        }
        std::vector<std::pair<std::string, uint64_t>> sorted(
            m_samplesByName.begin(), m_samplesByName.end());
        std::sort(sorted.begin(), sorted.end(),
                  [](const auto& a, const auto& b) { return a.second > b.second; });
        spdlog::info("PROF_TOP: {} samples total ({} unique functions) — self-time CPU profile:",
                     m_totalSamples, sorted.size());
        const size_t limit = std::min<size_t>(m_topN, sorted.size());
        for (size_t i = 0; i < limit; ++i) {
            const double pct = 100.0 * static_cast<double>(sorted[i].second) /
                               static_cast<double>(m_totalSamples);
            spdlog::info("PROF_TOP #{:>2} {:>5.1f}%  ({:>6} samples)  {}",
                         i + 1, pct, sorted[i].second, sorted[i].first);
        }
        if (m_captureStacks) {
            spdlog::info("PROF_CALLERS: dominant callers of the top self-time leaves:");
            for (size_t i = 0; i < limit; ++i) {
                auto it = m_callersByLeaf.find(sorted[i].first);
                if (it == m_callersByLeaf.end()) {
                    continue;
                }
                std::vector<std::pair<std::string, uint32_t>> callers(
                    it->second.begin(), it->second.end());
                std::sort(callers.begin(), callers.end(),
                          [](const auto& a, const auto& b) { return a.second > b.second; });
                uint32_t callerTotal = 0;
                for (const auto& kv : callers) {
                    callerTotal += kv.second;
                }
                std::string top;
                const size_t k = std::min<size_t>(4, callers.size());
                for (size_t j = 0; j < k; ++j) {
                    char buf[200];
                    std::snprintf(buf, sizeof(buf), " %s=%.0f%%", callers[j].first.c_str(),
                                  100.0 * callers[j].second /
                                      static_cast<double>(std::max<uint32_t>(1u, callerTotal)));
                    top += buf;
                }
                spdlog::info("PROF_CALLERS #{:>2} {}  <-{}", i + 1, sorted[i].first, top);
            }
        }
    }

    void DumpHitchFrame(uint32_t frame, double frameMs,
                        const std::unordered_map<std::string, uint32_t>& hist) {
        std::vector<std::pair<std::string, uint32_t>> sorted(hist.begin(), hist.end());
        std::sort(sorted.begin(), sorted.end(),
                  [](const auto& a, const auto& b) { return a.second > b.second; });
        uint32_t total = 0;
        for (const auto& kv : sorted) {
            total += kv.second;
        }
        std::string top;
        const size_t k = std::min<size_t>(6, sorted.size());
        for (size_t i = 0; i < k; ++i) {
            char buf[160];
            std::snprintf(buf, sizeof(buf), " %s=%.0f%%", sorted[i].first.c_str(),
                          100.0 * static_cast<double>(sorted[i].second) /
                              static_cast<double>(std::max<uint32_t>(1u, total)));
            top += buf;
        }
        spdlog::warn("PROF_HITCH frame={} ~{:.0f}ms ({} samples) top:{}",
                     frame, frameMs, total, top);
    }

    bool m_enabled = false;
    std::atomic<bool> m_running{false};
    std::atomic<uint32_t> m_currentFrame{0};
    HANDLE m_mainThread = nullptr;
    std::thread m_thread;
    uint32_t m_stallMs = 30;
    uint32_t m_hitchMs = 40;
    uint32_t m_intervalMs = 1;
    uint32_t m_topN = 30;
    bool m_captureStacks = false;
    // leaf function name -> { caller name -> sample count }; only when m_captureStacks.
    std::unordered_map<std::string, std::unordered_map<std::string, uint32_t>> m_callersByLeaf;
    uint64_t m_totalSamples = 0;
    std::unordered_map<std::string, uint64_t> m_samplesByName;  // sampling-thread only
    std::unordered_map<DWORD64, std::string> m_nameByBase;       // sampling-thread only
};

}  // namespace VENPOD::Tools
