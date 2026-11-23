// Virtual-time models shared by the timing tests and the latency simulation.
#pragma once
#include <algorithm>
#include <deque>
#include <random>
#include <vector>

#include "timing/frame_governor.h"

namespace bll::sim {

// OS timer model: sleeps wake late by a random amount, spinning costs time.
class FakeClock : public WaitBackend {
public:
    Ticks t = 1 * kNsPerSec;
    Ticks late_min = 0;
    Ticks late_max = 0;
    Ticks pause_cost = 200; // ns per spin iteration
    uint64_t sleeps = 0;
    uint64_t pauses = 0;
    std::mt19937 rng{42};

    Ticks now() override { return t; }
    void sleep_until(Ticks wake) override {
        ++sleeps;
        Ticks late = late_min;
        if (late_max > late_min) late += static_cast<Ticks>(rng() % static_cast<uint64_t>(late_max - late_min));
        t = std::max(t, wake + late);
    }
    void pause() override {
        ++pauses;
        t += pause_cost;
    }
};

// A refresh-locked flip-model presentation queue, following the documented
// DXGI semantics:
//   - a presented frame is shown at the first vblank after it finished on the
//     GPU (plus a compositor lead time), one frame per vblank, in order;
//   - at most `max_latency` frames may be presented-but-not-yet-shown;
//   - without a waitable swap chain, Present blocks while the queue is full;
//   - with one, Present returns at once and the waitable object is signalled
//     whenever the queue has room.
// This is a model of the API contract, not a measurement of any real system.
class DisplayQueue : public GateDevice {
public:
    DisplayQueue(FakeClock& clock, Ticks period, int max_latency, bool waitable)
        : clock_(clock), period_(period), max_latency_(max_latency), waitable_(waitable) {}

    Ticks lead = 1 * kNsPerMs; // compositor / flip deadline before the vblank

    // The game's Present call. Returns the vblank at which the frame is shown.
    Ticks present(Ticks gpu_work) {
        // The GPU starts on the submitted commands even while Present blocks.
        gpu_done_ = std::max(clock_.t, gpu_done_) + gpu_work;
        if (!waitable_) block_until_room();
        Ticks earliest = std::max(gpu_done_, clock_.t) + lead;
        if (!shown_.empty()) earliest = std::max(earliest, shown_.back() + period_);
        const Ticks vblank = (earliest + period_ - 1) / period_ * period_;
        shown_.push_back(vblank);
        return vblank;
    }

    Wait wait_frame_latency(Ticks timeout) override {
        if (!waitable_) return Wait::Unavailable;
        const Ticks before = clock_.t;
        block_until_room();
        if (clock_.t - before > timeout) {
            clock_.t = before + timeout;
            return Wait::Timeout;
        }
        return Wait::Signaled;
    }

    bool gpu_frame_pending() override { return gpu_done_ > clock_.t; }

private:
    void block_until_room() {
        while (!shown_.empty() && shown_.front() <= clock_.t) shown_.pop_front();
        while (static_cast<int>(shown_.size()) >= max_latency_) {
            clock_.t = std::max(clock_.t, shown_.front());
            shown_.pop_front();
        }
    }

    FakeClock& clock_;
    Ticks period_;
    int max_latency_;
    bool waitable_;
    Ticks gpu_done_ = 0;
    std::deque<Ticks> shown_;
};

struct LoopResult {
    std::vector<double> input_to_display_ms; // game's input poll -> vblank showing that frame
    std::vector<double> frame_interval_ms;
    size_t repeated_refreshes = 0; // vblanks without a new frame after start-up
    Ticks spin_total = 0;
    Ticks elapsed = 0;
};

struct Workload {
    Ticks cpu_mean = 3 * kNsPerMs;
    Ticks cpu_jitter = 500 * kNsPerUs;
    Ticks gpu_mean = 2 * kNsPerMs;
    Ticks gpu_jitter = 500 * kNsPerUs;
    // Occasional long frames (chunk rebuilds and the like).
    int spike_every = 0;
    Ticks spike_extra = 0;
};

// Runs a game loop shaped like "poll input, simulate + render, Present" against
// the display model with the governor attached exactly as the DXGI hook does.
inline LoopResult run_game_loop(const Config& cfg, Ticks period, int max_latency, bool waitable_chain,
                                const Workload& w, int frames, unsigned seed = 7) {
    FakeClock clock;
    clock.late_max = 300 * kNsPerUs;
    DisplayQueue display(clock, period, max_latency, waitable_chain);
    FrameGovernor governor(cfg, clock);
    governor.set_refresh_period(period);
    if (!waitable_chain) governor.set_waitable_enabled(false);

    std::mt19937 rng(seed);
    auto jitter = [&](Ticks mean, Ticks j) {
        return j ? mean - j + static_cast<Ticks>(rng() % static_cast<uint64_t>(2 * j)) : mean;
    };

    LoopResult out;
    const Ticks begin = clock.t;
    Ticks prev_vblank = 0;
    Ticks prev_call = 0;
    const int warmup = frames / 4;
    for (int i = 0; i < frames; ++i) {
        const Ticks poll = clock.t;
        governor.note_input_poll(poll);

        Ticks cpu = jitter(w.cpu_mean, w.cpu_jitter);
        if (w.spike_every && i % w.spike_every == w.spike_every - 1) cpu += w.spike_extra;
        clock.t += cpu;

        PresentSample s;
        s.call = clock.t;
        s.sync_interval = 1;
        const Ticks vblank = display.present(jitter(w.gpu_mean, w.gpu_jitter));
        clock.t += 20 * kNsPerUs; // cost of the call itself
        s.ret = clock.t;
        governor.on_present(display, s);

        if (i >= warmup) {
            out.input_to_display_ms.push_back(to_ms(vblank - poll));
            out.frame_interval_ms.push_back(to_ms(s.call - prev_call));
            if (vblank - prev_vblank > period) ++out.repeated_refreshes;
        }
        prev_vblank = vblank;
        prev_call = s.call;
    }
    out.spin_total = governor.sleeper().total_spin();
    out.elapsed = clock.t - begin;
    return out;
}

inline double median(std::vector<double> v) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

inline double percentile(std::vector<double> v, double p) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    const size_t k = std::min(v.size() - 1, static_cast<size_t>(p / 100.0 * static_cast<double>(v.size())));
    return v[k];
}

} // namespace bll::sim
