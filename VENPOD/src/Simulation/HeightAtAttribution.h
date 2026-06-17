#pragma once

// =============================================================================
// Manual caller attribution for SparseTerrainGenerator::HeightAt (header-only).
//
// Stack-walk profiling on optimized code mis-attributes the HeightAt caller. This
// instead makes every HeightAt call self-report which CALLER it came from, via a
// scoped tag (HEIGHTAT_SCOPE) the hot call sites push. Per render-thread call it
// records: count + UNIQUE (x,z) per caller. calls >> uniqueXZ for a caller == that
// caller is sampling the same columns repeatedly (cache it); calls ~= voxel-count
// rather than column-count == it is calling HeightAt PER VOXEL (the poison — give
// it a precomputed HeightBlock). Calls with no scope land in "<anonymous>" so no
// caller can hide. Only RENDER-THREAD (main) calls are counted; worker gen is fine.
//
// Enable: VENPOD_HEIGHTAT_ATTRIB=1. Dumped per frame (HEIGHTAT_ATTRIB) + per hitch.
// =============================================================================

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <spdlog/spdlog.h>

namespace VENPOD::Simulation {

// Symbolize a code address to "module!Function+0xNN" (file:line if available).
// Defined out-of-line in HeightAtAttribution.cpp so dbghelp/windows.h stays out of
// this header (it is included by TUs that use `near`/`far` as identifiers).
std::string HeightAtSymbolize(const void* addr);

class HeightAtAttribution {
public:
    struct Entry {
        uint64_t calls = 0;
        std::unordered_set<uint64_t> uniqueXZ;
    };

    // A caller above this calls/unique ratio is re-sampling columns (per-voxel poison),
    // not doing a wide unique sweep. ~1.0x = healthy; >=3x = regression.
    static constexpr double kTripwireRatio = 3.0;

    static void Init() {
        const char* e = std::getenv("VENPOD_HEIGHTAT_ATTRIB");
        s_enabled() = (e != nullptr && e[0] != '0' && e[0] != '\0');
        s_mainThreadId() = std::this_thread::get_id();
        // Regression tripwire: if any single caller exceeds this many main-thread
        // HeightAt calls in one frame, it has almost certainly regressed to per-voxel
        // sampling (the poison this refactor removed). 0 = off. Only active alongside
        // attribution (the dev/CI harness), so production pays nothing.
        const char* t = std::getenv("VENPOD_HEIGHTAT_TRIPWIRE");
        s_tripwire() = (t != nullptr) ? static_cast<uint64_t>(std::strtoull(t, nullptr, 10)) : 50000ull;
    }
    static bool Enabled() { return s_enabled(); }

    static void Push(const char* tag) { Stack().push_back(tag); }
    static void Pop() { if (!Stack().empty()) Stack().pop_back(); }

    // Called from HeightAt. Render-thread only -> single-writer, no lock.
    // retAddr = HeightAt's own return address (its immediate caller); for unscoped
    // (anonymous) calls we bucket it so the dump can symbolize WHO actually called.
    static void RecordHeight(int32_t x, int32_t z, const void* retAddr = nullptr) {
        if (!s_enabled() || std::this_thread::get_id() != s_mainThreadId()) {
            return;
        }
        const char* tag = Stack().empty() ? "<anonymous>" : Stack().back();
        Entry& en = ByCaller()[tag];
        ++en.calls;
        en.uniqueXZ.insert((static_cast<uint64_t>(static_cast<uint32_t>(x)) << 32) |
                           static_cast<uint32_t>(z));
        if (Stack().empty() && retAddr != nullptr) {
            ++AnonAddrs()[retAddr];
        }
    }
    static void RecordNoise() {
        if (!s_enabled() || std::this_thread::get_id() != s_mainThreadId()) {
            return;
        }
        ++NoiseCalls();
    }

    static void DumpAndReset(uint64_t frame, bool hitch) {
        if (!s_enabled() || ByCaller().empty()) {
            ByCaller().clear();
            AnonAddrs().clear();
            NoiseCalls() = 0;
            return;
        }
        uint64_t total = 0;
        for (auto& kv : ByCaller()) {
            total += kv.second.calls;
        }
        std::vector<std::pair<std::string, Entry*>> v;
        v.reserve(ByCaller().size());
        for (auto& kv : ByCaller()) {
            v.push_back({kv.first, &kv.second});
        }
        std::sort(v.begin(), v.end(),
                  [](const auto& a, const auto& b) { return a.second->calls > b.second->calls; });
        std::string line;
        for (size_t i = 0; i < v.size() && i < 10; ++i) {
            char buf[256];
            // calls(uniq=N,x=calls/uniq): x>>1 means redundant re-sampling of columns.
            const size_t uniq = std::max<size_t>(1, v[i].second->uniqueXZ.size());
            std::snprintf(buf, sizeof(buf), " %s=%llu(uniq=%zu,%.1fx)", v[i].first.c_str(),
                          static_cast<unsigned long long>(v[i].second->calls),
                          v[i].second->uniqueXZ.size(),
                          static_cast<double>(v[i].second->calls) / static_cast<double>(uniq));
            line += buf;
        }
        spdlog::warn("HEIGHTAT_ATTRIB frame={}{} renderThreadCalls={} noisePerHeight={:.1f} byCaller:{}",
                     frame, hitch ? " HITCH" : "", total,
                     total ? static_cast<double>(NoiseCalls()) / static_cast<double>(total) : 0.0,
                     line);
        // Tripwire: poison is RE-SAMPLING the same columns (calls >> unique), not a
        // legitimately-large unique sweep (e.g. CPU raymarch touches many unique
        // columns at ~1.0x). So fire only when a caller passes the per-frame floor AND
        // its calls/unique ratio shows per-voxel recompute. Loud, attributed, precise.
        for (size_t i = 0; i < v.size(); ++i) {
            if (v[i].second->calls <= s_tripwire()) {
                break;  // sorted desc; nothing below clears the floor
            }
            const size_t uniq = std::max<size_t>(1, v[i].second->uniqueXZ.size());
            const double ratio = static_cast<double>(v[i].second->calls) / static_cast<double>(uniq);
            if (ratio >= kTripwireRatio) {
                spdlog::error("HEIGHTAT_TRIPWIRE frame={} caller={} calls={} (uniq={}, {:.1f}x) > "
                              "floor={} at >={:.1f}x - likely regressed to PER-VOXEL HeightAt",
                              frame, v[i].first.c_str(),
                              static_cast<unsigned long long>(v[i].second->calls),
                              v[i].second->uniqueXZ.size(), ratio,
                              static_cast<unsigned long long>(s_tripwire()), kTripwireRatio);
            }
        }
        // Resolve the dominant ANONYMOUS callers (unscoped HeightAt) by return address
        // so no caller can hide. Only the top few, only on busy frames, to bound cost.
        if (!AnonAddrs().empty()) {
            std::vector<std::pair<const void*, uint64_t>> av(AnonAddrs().begin(), AnonAddrs().end());
            std::sort(av.begin(), av.end(),
                      [](const auto& a, const auto& b) { return a.second > b.second; });
            std::string aline;
            for (size_t i = 0; i < av.size() && i < 5; ++i) {
                char buf[64];
                std::snprintf(buf, sizeof(buf), " [%llu] ",
                              static_cast<unsigned long long>(av[i].second));
                aline += buf;
                aline += HeightAtSymbolize(av[i].first);
            }
            spdlog::warn("HEIGHTAT_ATTRIB_ANON frame={} topCallers:{}", frame, aline);
        }
        ByCaller().clear();
        AnonAddrs().clear();
        NoiseCalls() = 0;
    }

private:
    static bool& s_enabled() { static bool v = false; return v; }
    static uint64_t& s_tripwire() { static uint64_t v = 0; return v; }
    static std::thread::id& s_mainThreadId() { static std::thread::id v; return v; }
    static uint64_t& NoiseCalls() { static uint64_t v = 0; return v; }  // main-thread only
    static std::unordered_map<std::string, Entry>& ByCaller() {         // main-thread only
        static std::unordered_map<std::string, Entry> m;
        return m;
    }
    static std::unordered_map<const void*, uint64_t>& AnonAddrs() {     // main-thread only
        static std::unordered_map<const void*, uint64_t> m;
        return m;
    }
    static std::vector<const char*>& Stack() {
        static thread_local std::vector<const char*> s;
        return s;
    }
};

struct HeightAtCallerScope {
    bool active;
    explicit HeightAtCallerScope(const char* tag) : active(HeightAtAttribution::Enabled()) {
        if (active) {
            HeightAtAttribution::Push(tag);
        }
    }
    ~HeightAtCallerScope() {
        if (active) {
            HeightAtAttribution::Pop();
        }
    }
    HeightAtCallerScope(const HeightAtCallerScope&) = delete;
    HeightAtCallerScope& operator=(const HeightAtCallerScope&) = delete;
};

}  // namespace VENPOD::Simulation

#define HEIGHTAT_CONCAT_(a, b) a##b
#define HEIGHTAT_CONCAT(a, b) HEIGHTAT_CONCAT_(a, b)
#define HEIGHTAT_SCOPE(tag) \
    ::VENPOD::Simulation::HeightAtCallerScope HEIGHTAT_CONCAT(heightAtScope_, __LINE__)(tag)
