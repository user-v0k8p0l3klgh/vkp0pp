// Deadline-based frame limiter. It schedules frame *starts*: the wait happens
// after Present returns and before the game samples input for the next frame,
// so limiting never ages an already rendered frame.
#pragma once
#include "core/time.h"

namespace bll {

class FramePacer {
public:
    // 0 disables pacing.
    void set_interval(Ticks interval);
    Ticks interval() const { return interval_; }

    // Called at a frame boundary. Returns the time the next frame should start
    // at; a value <= now means "start immediately". 0 when pacing is disabled.
    Ticks next_start(Ticks now);

    void reset() { next_ = 0; }

private:
    Ticks interval_ = 0;
    Ticks next_ = 0;
};

} // namespace bll
