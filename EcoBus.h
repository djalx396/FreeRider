// EcoBus.h — The Ecosystem Bus, layout v1.
//
// One shared module for every Ecosystem plug-in (Console, FreeSpacer, Free Rider, ...).
// Copy this folder unchanged into each project; plug-ins built from the same layout
// version share one bus, even though they are different binaries.
//
// What it is
//   * 24 channels (1-24, 0 = off). Any number of senders per channel are summed;
//     any number of listeners can read a channel.
//   * Payload is stereo audio at the session rate (each listener derives what it needs).
//   * Every frame is stored at its timeline sample position, never "the latest block":
//     a listener reads exactly the positions its own output corresponds to.
//   * No feedback: a listener never hears its own sender slot.
//   * Global channel names (01-24 until renamed); the most recently edited table wins.
//
// How it is shared
//   The bus lives in anonymous memory owned by the process (never freed). Its address
//   is published in an environment variable tagged with the process id, so every
//   plug-in binary loaded into the same process finds the same bus, a child process
//   that inherits the variable ignores it, and nothing is left behind after the
//   process exits. A file lock (/tmp/ecosystem-bus-v1.lock) serialises creation.
//   Plug-ins running in a separate (bridged/sandboxed) process get their own bus.
//
// Threading
//   attach(), claim/release and names: message thread (or prepareToPlay).
//   write()/read(): audio thread, lock-free, no allocation.
#pragma once
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <unistd.h>

namespace eco {

constexpr uint32_t kMagic         = 0x45434f31u;  // "ECO1"
constexpr uint32_t kLayoutVersion = 1;
constexpr int      kNumChannels   = 24;
constexpr int      kNameLen       = 32;
constexpr int      kSlots         = 64;           // concurrent senders per process
constexpr int      kCapLog2       = 17;           // 131072 frames per sender ring
constexpr int      kCap           = 1 << kCapLog2;
constexpr int      kMask          = kCap - 1;
constexpr int64_t  kPosOffset     = int64_t (1) << 40;  // keeps pre-roll (negative) positions valid
constexpr int64_t  kStaleMs       = 2000;         // sender not seen for this long = gone
constexpr int64_t  kReclaimMs     = 10000;        // orphaned slot (crashed instance) can be reused

static_assert (std::atomic<uint32_t>::is_always_lock_free && std::atomic<int64_t>::is_always_lock_free
               && std::atomic<uint64_t>::is_always_lock_free, "EcoBus needs lock-free atomics");

inline int64_t nowMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds> (steady_clock::now().time_since_epoch()).count();
}
inline int64_t wallMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds> (system_clock::now().time_since_epoch()).count();
}

struct Slot
{
    std::atomic<uint32_t> state;        // 0 free, 1 claimed
    std::atomic<uint32_t> channel;      // 0 = not sending, 1..24
    std::atomic<uint64_t> owner;        // instance id
    std::atomic<uint32_t> sampleRate;   // Hz
    std::atomic<int64_t>  heartbeatMs;  // steady clock, updated by the owner's timer
    std::atomic<uint32_t> tag[kCap];    // lap number + 1 of the frame stored at each index
    float l[kCap];
    float r[kCap];
};

struct Names
{
    std::atomic<uint32_t> seq;          // seqlock: odd while being written
    std::atomic<int64_t>  editedMs;     // wall clock of the last rename (0 = never)
    char name[kNumChannels][kNameLen];  // empty = default "01".."24"
};

struct Shared
{
    std::atomic<uint32_t> magic;
    uint32_t version;
    uint64_t size;
    std::atomic<uint64_t> nextId;
    Names names;
    Slot slots[kSlots];
};

//==============================================================================
inline Shared* attach()
{
    static std::atomic<Shared*> cached { nullptr };   // per binary; the bus itself is per process
    if (auto* s = cached.load (std::memory_order_acquire)) return s;

    static const char* kEnv = "ECOSYSTEM_BUS_V1";
    Shared* s = nullptr;
    const int lockFd = ::open ("/tmp/ecosystem-bus-v1.lock", O_RDWR | O_CREAT, 0666);
    if (lockFd >= 0) ::flock (lockFd, LOCK_EX);

    if (const char* env = std::getenv (kEnv))
    {
        long long pid = 0; unsigned long long addr = 0;
        if (std::sscanf (env, "%lld:%llx", &pid, &addr) == 2 && pid == (long long) ::getpid() && addr != 0)
            s = reinterpret_cast<Shared*> ((uintptr_t) addr);
    }
    if (s == nullptr)
    {
        void* m = ::mmap (nullptr, sizeof (Shared), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (m != MAP_FAILED)
        {
            s = static_cast<Shared*> (m);            // zero-filled by the OS
            s->version = kLayoutVersion;
            s->size = sizeof (Shared);
            s->magic.store (kMagic, std::memory_order_release);
            char buf[64];
            std::snprintf (buf, sizeof (buf), "%lld:%llx", (long long) ::getpid(), (unsigned long long) (uintptr_t) s);
            ::setenv (kEnv, buf, 1);
        }
    }
    if (lockFd >= 0) { ::flock (lockFd, LOCK_UN); ::close (lockFd); }

    if (s != nullptr && (s->magic.load (std::memory_order_acquire) != kMagic
                         || s->version != kLayoutVersion || s->size != sizeof (Shared)))
        s = nullptr;
    cached.store (s, std::memory_order_release);
    return s;
}

inline uint64_t newInstanceId (Shared* s) { return s ? s->nextId.fetch_add (1) + 1 : 0; }

//==============================================================================
// Channel names
inline void defaultName (int ch, char* out) { std::snprintf (out, kNameLen, "%02d", ch); }

// Copies all 24 names (empty = default). Returns the edit stamp of the copied table.
inline int64_t readNames (Shared* s, char out[kNumChannels][kNameLen])
{
    if (s == nullptr) { std::memset (out, 0, kNumChannels * kNameLen); return 0; }
    for (int attempt = 0; attempt < 100; ++attempt)
    {
        const uint32_t a = s->names.seq.load (std::memory_order_acquire);
        if (a & 1u) continue;
        std::memcpy (out, s->names.name, kNumChannels * kNameLen);
        const int64_t stamp = s->names.editedMs.load (std::memory_order_relaxed);
        std::atomic_thread_fence (std::memory_order_acquire);
        if (s->names.seq.load (std::memory_order_relaxed) == a) return stamp;
    }
    return 0;
}

inline void writeNames (Shared* s, const char in[kNumChannels][kNameLen], int64_t stamp)
{
    if (s == nullptr) return;
    uint32_t a = s->names.seq.load();
    while ((a & 1u) || ! s->names.seq.compare_exchange_weak (a, a + 1)) a = s->names.seq.load();
    std::memcpy (s->names.name, in, kNumChannels * kNameLen);
    for (int c = 0; c < kNumChannels; ++c) s->names.name[c][kNameLen - 1] = 0;
    s->names.editedMs.store (stamp);
    s->names.seq.store (a + 2, std::memory_order_release);
}

// Rename one channel (ch 1..24; empty name = back to default).
inline void renameChannel (Shared* s, int ch, const char* name)
{
    if (s == nullptr || ch < 1 || ch > kNumChannels) return;
    char t[kNumChannels][kNameLen];
    readNames (s, t);
    std::memset (t[ch - 1], 0, kNameLen);
    std::strncpy (t[ch - 1], name, kNameLen - 1);
    writeNames (s, t, wallMs());
}

// A loaded project offers its saved table; it wins only if it was edited more recently.
inline bool offerNames (Shared* s, const char in[kNumChannels][kNameLen], int64_t stamp)
{
    if (s == nullptr || stamp <= s->names.editedMs.load()) return false;
    writeNames (s, in, stamp);
    return true;
}

//==============================================================================
// Sender: one per plug-in instance that publishes. Claim on the message thread,
// write on the audio thread.
class Sender
{
public:
    ~Sender() { release(); }

    // Returns false if all slots are in use.
    bool claim (Shared* s, uint64_t ownerId, uint32_t sampleRate)
    {
        bus = s;
        if (bus == nullptr) return false;
        if (slot >= 0) { setRate (sampleRate); return true; }
        const int64_t now = nowMs();
        for (int pass = 0; pass < 2; ++pass)
            for (int i = 0; i < kSlots; ++i)
            {
                Slot& sl = bus->slots[i];
                uint32_t expected = 0;
                bool got = pass == 0 && sl.state.compare_exchange_strong (expected, 1u);
                if (! got && pass == 1)
                {
                    // Orphaned by an instance that died without releasing (its heartbeat
                    // stopped long ago): take it over atomically via the heartbeat.
                    int64_t hb = sl.heartbeatMs.load();
                    got = sl.state.load() == 1u && now - hb > kReclaimMs
                          && sl.heartbeatMs.compare_exchange_strong (hb, now);
                }
                if (got)
                {
                    sl.channel.store (0);
                    sl.owner.store (ownerId);
                    sl.sampleRate.store (sampleRate);
                    sl.heartbeatMs.store (now);
                    slot = i;
                    return true;
                }
            }
        return false;
    }
    void release()
    {
        if (bus != nullptr && slot >= 0)
        {
            Slot& sl = bus->slots[slot];
            sl.channel.store (0);
            sl.owner.store (0);
            sl.state.store (0, std::memory_order_release);
        }
        slot = -1;
    }
    void setChannel (int ch) { if (bus && slot >= 0) bus->slots[slot].channel.store ((uint32_t) (ch >= 1 && ch <= kNumChannels ? ch : 0)); }
    void setRate (uint32_t sr) { if (bus && slot >= 0) bus->slots[slot].sampleRate.store (sr); }
    void heartbeat() { if (bus && slot >= 0) bus->slots[slot].heartbeatMs.store (nowMs()); }
    bool active() const { return bus != nullptr && slot >= 0; }
    int  slotIndex() const { return slot; }

    // Audio thread: frames for timeline positions pos .. pos+n-1. R may equal L (mono).
    void write (int64_t pos, const float* L, const float* R, int n) noexcept
    {
        if (bus == nullptr || slot < 0) return;
        Slot& sl = bus->slots[slot];
        for (int i = 0; i < n; ++i)
        {
            const int64_t p = pos + i + kPosOffset;
            const int idx = (int) (p & kMask);
            sl.l[idx] = L[i];
            sl.r[idx] = R[i];
            sl.tag[idx].store ((uint32_t) (p >> kCapLog2) + 1u, std::memory_order_release);
        }
    }

private:
    Shared* bus = nullptr;
    int slot = -1;
};

//==============================================================================
struct ReadResult
{
    int senders = 0;      // live senders on the channel (excluding self)
    int missing = 0;      // frames a live sender hadn't written yet (arrived too late)
};

// Audio thread: sums every live sender on `ch` for positions pos .. pos+n-1 into L/R
// (overwrites L/R). `selfOwner` is skipped (no feedback).
inline ReadResult read (Shared* s, int ch, int64_t pos, int n, uint32_t sampleRate, uint64_t selfOwner,
                        float* L, float* R) noexcept
{
    ReadResult res;
    std::memset (L, 0, sizeof (float) * (size_t) n);
    std::memset (R, 0, sizeof (float) * (size_t) n);
    if (s == nullptr || ch < 1 || ch > kNumChannels) return res;
    const int64_t now = nowMs();
    for (int i = 0; i < kSlots; ++i)
    {
        Slot& sl = s->slots[i];
        if (sl.state.load (std::memory_order_acquire) != 1u) continue;
        if ((int) sl.channel.load (std::memory_order_relaxed) != ch) continue;
        if (sl.owner.load (std::memory_order_relaxed) == selfOwner) continue;
        if (sl.sampleRate.load (std::memory_order_relaxed) != sampleRate) continue;
        if (now - sl.heartbeatMs.load (std::memory_order_relaxed) > kStaleMs) continue;
        ++res.senders;
        for (int k = 0; k < n; ++k)
        {
            const int64_t p = pos + k + kPosOffset;
            const int idx = (int) (p & kMask);
            if (sl.tag[idx].load (std::memory_order_acquire) == (uint32_t) (p >> kCapLog2) + 1u)
            {
                L[k] += sl.l[idx];
                R[k] += sl.r[idx];
            }
            else ++res.missing;
        }
    }
    return res;
}

// Message thread: how many live senders are on each channel (for "found" displays).
inline int countSenders (Shared* s, int ch, uint64_t selfOwner)
{
    if (s == nullptr) return 0;
    int c = 0; const int64_t now = nowMs();
    for (int i = 0; i < kSlots; ++i)
    {
        Slot& sl = s->slots[i];
        if (sl.state.load() == 1u && (int) sl.channel.load() == ch && sl.owner.load() != selfOwner
            && now - sl.heartbeatMs.load() <= kStaleMs) ++c;
    }
    return c;
}

} // namespace eco
