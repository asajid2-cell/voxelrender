#pragma once

// =============================================================================
// VENPOD deterministic run record/replay (header-only).
//
// Captures the camera path (and brush intent) per LOGICAL frame so a real
// interactive session -- e.g. "fly out to the mountains and edit" -- can be
// replayed byte-for-byte on the input side, giving a reproducible profiling
// scenario and clean A/B (the only variable between two replays of the same
// recording is the code under test, not where the camera happened to fly).
//
// The camera is the dominant cost driver (dense terrain in view), so v1 drives
// the camera on replay; brush intent is recorded alongside for completeness.
//
//   Record:  set VENPOD_RECORD=<path>   (e.g. via recrun.ps1) -> writes <path>.
//   Replay:  set VENPOD_REPLAY=<path>   -> drives the camera from <path>.
//
// File: [u32 magic 'VNRD'][u32 frameCount][Frame[frameCount]], Frame = 32 bytes.
// =============================================================================

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

namespace VENPOD::Tools {

class RunRecorder {
public:
    struct Frame {
        float posX = 0.0f;
        float posY = 0.0f;
        float posZ = 0.0f;
        float yaw = 0.0f;
        float pitch = 0.0f;
        uint32_t material = 0u;
        float brushRadius = 0.0f;
        uint32_t flags = 0u;  // bit0 = painting, bit1 = erasing
    };
    static_assert(sizeof(Frame) == 32, "RunRecorder::Frame must be 32 bytes");

    static constexpr uint32_t kFlagPainting = 1u << 0;
    static constexpr uint32_t kFlagErasing = 1u << 1;

    void Initialize() {
        const char* rec = std::getenv("VENPOD_RECORD");
        const char* rep = std::getenv("VENPOD_REPLAY");
        if (rec != nullptr && rec[0] != '\0') {
            m_mode = Mode::Record;
            m_path = rec;
        } else if (rep != nullptr && rep[0] != '\0') {
            m_mode = Mode::Replay;
            m_path = rep;
            LoadReplay();
        }
    }

    bool IsRecord() const { return m_mode == Mode::Record; }
    bool IsReplay() const { return m_mode == Mode::Replay; }
    bool IsActive() const { return m_mode != Mode::Off; }
    uint64_t ReplayFrameCount() const { return static_cast<uint64_t>(m_frames.size()); }

    // Record: append this frame's final camera + brush intent (call once per frame).
    void RecordFrame(const Frame& f) {
        if (m_mode == Mode::Record) {
            m_frames.push_back(f);
        }
    }

    // Replay: fetch the recorded frame for frameIndex. Returns false past the end
    // (caller can then exit / stop driving the camera).
    bool GetReplayFrame(uint64_t frameIndex, Frame* out) const {
        if (m_mode != Mode::Replay || out == nullptr ||
            frameIndex >= static_cast<uint64_t>(m_frames.size())) {
            return false;
        }
        *out = m_frames[static_cast<size_t>(frameIndex)];
        return true;
    }

    // Flush the recording to disk (call once at shutdown / exit).
    void Shutdown() {
        if (m_mode != Mode::Record || m_frames.empty() || m_flushed) {
            return;
        }
        std::ofstream f(m_path, std::ios::binary | std::ios::trunc);
        if (!f) {
            return;
        }
        const uint32_t magic = kMagic;
        const uint32_t count = static_cast<uint32_t>(m_frames.size());
        f.write(reinterpret_cast<const char*>(&magic), sizeof(magic));
        f.write(reinterpret_cast<const char*>(&count), sizeof(count));
        f.write(reinterpret_cast<const char*>(m_frames.data()),
                static_cast<std::streamsize>(m_frames.size() * sizeof(Frame)));
        m_flushed = true;
    }

private:
    enum class Mode { Off, Record, Replay };
    static constexpr uint32_t kMagic = 0x564E5244u;  // 'VNRD'

    void LoadReplay() {
        std::ifstream f(m_path, std::ios::binary);
        if (!f) {
            return;
        }
        uint32_t magic = 0u;
        uint32_t count = 0u;
        f.read(reinterpret_cast<char*>(&magic), sizeof(magic));
        f.read(reinterpret_cast<char*>(&count), sizeof(count));
        if (magic != kMagic || count == 0u) {
            return;
        }
        m_frames.resize(count);
        f.read(reinterpret_cast<char*>(m_frames.data()),
               static_cast<std::streamsize>(static_cast<size_t>(count) * sizeof(Frame)));
    }

    Mode m_mode = Mode::Off;
    bool m_flushed = false;
    std::string m_path;
    std::vector<Frame> m_frames;
};

}  // namespace VENPOD::Tools
