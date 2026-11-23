#include "timing/frame_pacer.h"

#include <algorithm>

namespace bll {

void FramePacer::set_interval(Ticks interval) {
    interval = std::max<Ticks>(interval, 0);
    if (interval == interval_) return;
    interval_ = interval;
    next_ = 0;
}

Ticks FramePacer::next_start(Ticks now) {
    if (interval_ == 0) return 0;
    if (next_ == 0) next_ = now;
    const Ticks start = next_;
    // A slightly late frame keeps the grid (the next one is shortened); after a
    // stall longer than a frame the grid is re-anchored instead of bursting.
    next_ = now - next_ > interval_ ? now + interval_ : next_ + interval_;
    return start;
}

} // namespace bll
