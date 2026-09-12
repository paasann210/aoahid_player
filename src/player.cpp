// SPDX-License-Identifier: MIT
#include "aoahid_player/player.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace aoap {
namespace {

static_assert(std::atomic<double>::is_always_lock_free);
static_assert(std::atomic<int64_t>::is_always_lock_free);
static_assert(std::atomic<uint64_t>::is_always_lock_free);

constexpr uint64_t segment_bit = uint64_t{1} << 63;

uint64_t encode(const PlaybackPosition position) noexcept {
    const uint64_t time = static_cast<uint64_t>(std::max<int64_t>(position.time_ns, 0));
    return (position.segment == Segment::loop ? segment_bit : 0U) | (time & ~segment_bit);
}

PlaybackPosition decode(const uint64_t word) noexcept {
    return {(word & segment_bit) != 0U ? Segment::loop : Segment::intro,
            static_cast<int64_t>(word & ~segment_bit)};
}

int64_t scaled(const int64_t script_span, const double speed) noexcept {
    const double value = static_cast<double>(script_span) / speed;
    if (value >= 9.0e18)
        return INT64_MAX / 4;
    if (value <= -9.0e18)
        return INT64_MIN / 4;
    return static_cast<int64_t>(std::llround(value));
}

} // namespace

Player::Player(DeviceGroup& group, EventSink* sink) : group_(group), sink_(sink) {}

void Player::load(std::shared_ptr<const EventScript> script) {
    script_ = std::move(script);
    timeline_ = script_ ? build_timeline(*script_) : Timeline{};
    // A batch never holds more rows than the script, and the transitions a
    // seek builds never hold more than the tracked state; reserving here
    // keeps the playback loop free of allocation.
    staged_keys_.clear();
    staged_keys_.reserve(script_ ? script_->size() + 1 : 1);
    releases_.reserve(64);
    presses_.reserve(64);
}

// --- Controls -------------------------------------------------------------

void Player::post(const uint32_t request) noexcept {
    requests_.fetch_or(request, std::memory_order_acq_rel);
    wake_.notify();
}

void Player::stop() noexcept { post(request_stop); }

void Player::pause() noexcept {
    want_paused_.store(true, std::memory_order_release);
    post(request_pause);
}

void Player::resume() noexcept {
    want_paused_.store(false, std::memory_order_release);
    post(request_pause);
}

void Player::seek(const PlaybackPosition target) noexcept {
    seek_word_.store(encode(target), std::memory_order_release);
    post(request_seek);
}

void Player::set_speed(const double speed) noexcept {
    const double value = std::isfinite(speed) ? std::clamp(speed, min_speed, max_speed) : 1.0;
    speed_.store(value, std::memory_order_release);
    post(request_retime);
}

void Player::set_loop_limit(const int64_t loops) noexcept {
    loop_limit_.store(std::max<int64_t>(loops, 0), std::memory_order_relaxed);
}

int64_t Player::adjust_offset_ns(const int64_t delta) noexcept {
    const int64_t total = offset_ns_.fetch_add(delta, std::memory_order_acq_rel) + delta;
    // A pending wait recomputes its deadline with the new offset.
    wake_.notify();
    return total;
}

void Player::set_offset_ns(const int64_t offset) noexcept {
    offset_ns_.store(offset, std::memory_order_release);
    wake_.notify();
}

void Player::set_stop_at(const int64_t stop_at_ns) noexcept {
    stop_at_ns_.store(stop_at_ns, std::memory_order_release);
    // A pending wait recomputes against the new limit.
    wake_.notify();
}

PlaybackStatus Player::status() const noexcept {
    PlaybackStatus status;
    uint8_t state = 0;
    uint8_t segment = 0;
    int64_t anchor_real = 0;
    int64_t anchor_script = 0;
    int64_t offset_anchor = 0;
    int64_t duration = 0;
    double speed = 1.0;
    uint64_t loops = 0;
    // Seqlock read. The field loads are acquire so the closing sequence load
    // cannot move above them; no fences, which ThreadSanitizer can check.
    while (true) {
        const uint32_t before = seq_.load(std::memory_order_acquire);
        if ((before & 1U) != 0U)
            continue;
        state = pub_state_.load(std::memory_order_acquire);
        segment = pub_segment_.load(std::memory_order_acquire);
        anchor_real = pub_anchor_real_.load(std::memory_order_acquire);
        anchor_script = pub_anchor_script_.load(std::memory_order_acquire);
        offset_anchor = pub_offset_anchor_.load(std::memory_order_acquire);
        duration = pub_duration_.load(std::memory_order_acquire);
        speed = pub_speed_.load(std::memory_order_acquire);
        loops = pub_loops_.load(std::memory_order_acquire);
        if (seq_.load(std::memory_order_relaxed) == before)
            break;
    }
    status.state = static_cast<PlaybackState>(state);
    status.position.segment = static_cast<Segment>(segment);
    status.loops = loops;
    status.reports = reports_.load(std::memory_order_relaxed);
    int64_t time = anchor_script;
    if (status.state == PlaybackState::playing) {
        const int64_t shift = offset_ns_.load(std::memory_order_relaxed) - offset_anchor;
        const double elapsed = static_cast<double>(Timing::now_ns() - anchor_real - shift);
        time = anchor_script + static_cast<int64_t>(elapsed * speed);
    }
    status.position.time_ns = std::clamp<int64_t>(time, 0, std::max<int64_t>(duration, 0));
    return status;
}

void Player::publish(const PlaybackState state, const PlaybackPosition position) noexcept {
    // Seqlock write (one writer). The acquire exchange keeps the field stores
    // below the odd sequence; the release store publishes them.
    const uint32_t sequence = seq_.load(std::memory_order_relaxed);
    seq_.exchange(sequence + 1, std::memory_order_acquire);
    pub_state_.store(static_cast<uint8_t>(state), std::memory_order_relaxed);
    pub_segment_.store(static_cast<uint8_t>(position.segment), std::memory_order_relaxed);
    pub_anchor_real_.store(anchor_real_, std::memory_order_relaxed);
    pub_anchor_script_.store(position.time_ns, std::memory_order_relaxed);
    pub_offset_anchor_.store(offset_anchor_, std::memory_order_relaxed);
    pub_duration_.store(timeline_.duration(position.segment), std::memory_order_relaxed);
    pub_speed_.store(anchor_speed_, std::memory_order_relaxed);
    pub_loops_.store(loops_, std::memory_order_relaxed);
    seq_.store(sequence + 2, std::memory_order_release);
}

// --- Timeline -------------------------------------------------------------

int64_t Player::deadline_for(const int64_t script_time) const noexcept {
    const int64_t shift = offset_ns_.load(std::memory_order_relaxed) - offset_anchor_;
    return anchor_real_ + scaled(script_time - anchor_script_, anchor_speed_) + shift;
}

int64_t Player::script_time_at(const int64_t now) const noexcept {
    const int64_t shift = offset_ns_.load(std::memory_order_relaxed) - offset_anchor_;
    const double elapsed = static_cast<double>(now - anchor_real_ - shift);
    return anchor_script_ + static_cast<int64_t>(elapsed * anchor_speed_);
}

PlaybackPosition Player::clamp(PlaybackPosition position) const noexcept {
    position.time_ns =
        std::clamp<int64_t>(position.time_ns, 0, timeline_.duration(position.segment));
    return position;
}

void Player::retime(const int64_t now) {
    // Re-anchor at the current script position so a speed change applies
    // from here on without a jump.
    anchor_script_ = script_time_at(now);
    anchor_real_ = now;
    offset_anchor_ = offset_ns_.load(std::memory_order_relaxed);
    anchor_speed_ = speed_.load(std::memory_order_acquire);
    publish(PlaybackState::playing, {segment_, anchor_script_});
}

// --- Sending --------------------------------------------------------------

bool Player::conflicts(const uint64_t key) const noexcept {
    if (key == 0)
        return false;
    return std::find(staged_keys_.begin(), staged_keys_.end(), key) != staged_keys_.end();
}

void Player::flush_now() {
    if (group_.flush())
        reports_.store(reports_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    staged_keys_.clear();
}

aoahid_result Player::send(const EventPayload& payload) {
    aoahid_result result = group_.apply(payload);
    if (result == AOAHID_ERR_BUSY) {
        // libaoahid saw an unreported opposite edge that batch_key did not
        // predict; let the pending report land, then retry once.
        flush_now();
        result = group_.apply(payload);
    }
    if (result == AOAHID_OK) {
        live_.apply(payload);
        if (observer_ != nullptr)
            observer_->sent(payload);
    }
    return result;
}

void Player::settle(const InputState& target) {
    InputState::transition(live_, target, releases_, presses_);
    if (releases_.empty() && presses_.empty())
        return;
    flush_now();
    // Results are ignored: a profile that is not connected or a value the
    // Spec rejects was already reported when the row itself was played.
    for (const EventPayload& payload : releases_)
        static_cast<void>(send(payload));
    flush_now();
    for (const EventPayload& payload : presses_)
        static_cast<void>(send(payload));
    flush_now();
}

void Player::report_skip(const aoahid_result result, const EventPayload& payload) {
    if (sink_ == nullptr)
        return;
    const Profile profile = profile_of(payload);
    if (result == AOAHID_ERR_UNSUPPORTED) {
        // Said once per profile instead of once per row.
        const uint32_t bit = profile_bit(profile);
        if ((warned_profiles_ & bit) != 0U)
            return;
        warned_profiles_ |= bit;
        sink_->message(Severity::warning, std::string("The script uses the ") +
                                              describe_profiles(bit) +
                                              ", which is not connected; those rows are skipped.");
        return;
    }
    if (result == AOAHID_ERR_PARAM && !rejection_reported_) {
        rejection_reported_ = true;
        sink_->message(Severity::warning,
                       std::string("A ") + describe_profiles(profile_bit(profile)) +
                           " row does not fit the connected settings and was skipped. " +
                           group_.rejection_detail());
    }
}

void Player::execute(const EventRecord& record) {
    const uint64_t key = batch_key(record.payload);
    if (conflicts(key))
        flush_now();
    const aoahid_result result = send(record.payload);
    if (result == AOAHID_OK) {
        if (key != 0)
            staged_keys_.push_back(key);
        return;
    }
    // The row is dropped; its time still belongs to the timeline, so every
    // later row keeps its absolute deadline.
    report_skip(result, record.payload);
}

void Player::jump(const PlaybackPosition requested) {
    flush_now();
    const PlaybackPosition target = clamp(requested);
    if (target.segment == Segment::intro)
        loops_ = 0;

    // Rebuild the state uninterrupted playback would have at `target`. Every
    // row sets an absolute value, so one full pass over the loop body stands
    // for any number of completed iterations.
    target_.clear();
    const size_t row = timeline_.row_at(target.segment, target.time_ns);
    if (target.segment == Segment::intro) {
        for (size_t index = 0; index < row; ++index)
            target_.apply(script_->once_rows[index].payload);
    } else {
        for (const EventRecord& record : script_->once_rows)
            target_.apply(record.payload);
        if (loops_ > 0) {
            for (const EventRecord& record : script_->loop_rows)
                target_.apply(record.payload);
        }
        for (size_t index = 0; index < row; ++index)
            target_.apply(script_->loop_rows[index].payload);
    }
    settle(target_);

    segment_ = target.segment;
    index_ = row;
    cursor_ = target.time_ns;
    anchor_real_ = Timing::now_ns();
    anchor_script_ = target.time_ns;
    offset_anchor_ = offset_ns_.load(std::memory_order_relaxed);
    anchor_speed_ = speed_.load(std::memory_order_acquire);
    publish(PlaybackState::playing, target);
}

// --- Control handling -----------------------------------------------------

Player::Outcome Player::service() {
    const uint32_t pending = requests_.exchange(0U, std::memory_order_acq_rel);
    if ((pending & request_stop) != 0U)
        return Outcome::stop;
    Outcome outcome = Outcome::proceed;
    if ((pending & request_retime) != 0U)
        retime(Timing::now_ns());
    if ((pending & request_seek) != 0U) {
        jump(decode(seek_word_.load(std::memory_order_acquire)));
        outcome = Outcome::jumped;
    }
    if ((pending & request_pause) != 0U && want_paused_.load(std::memory_order_acquire)) {
        pause_position_ = outcome == Outcome::jumped
                              ? PlaybackPosition{segment_, cursor_}
                              : clamp({segment_, script_time_at(Timing::now_ns())});
        return hold_paused();
    }
    return outcome;
}

Player::Outcome Player::hold_paused() {
    // Send what is staged, then release everything so nothing stays held
    // while nobody knows how long the pause lasts.
    flush_now();
    settle(InputState{});
    paused_ = true;
    publish(PlaybackState::paused, pause_position_);

    while (true) {
        const uint32_t seen = wake_.epoch();
        const uint32_t pending = requests_.exchange(0U, std::memory_order_acq_rel);
        if ((pending & request_stop) != 0U) {
            paused_ = false;
            return Outcome::stop;
        }
        if ((pending & request_seek) != 0U) {
            pause_position_ = clamp(decode(seek_word_.load(std::memory_order_acquire)));
            if (pause_position_.segment == Segment::intro)
                loops_ = 0;
            publish(PlaybackState::paused, pause_position_);
        }
        if ((pending & request_pause) != 0U && !want_paused_.load(std::memory_order_acquire)) {
            paused_ = false;
            jump(pause_position_);
            return Outcome::jumped;
        }
        if (pending == 0U) {
            const int64_t stop_at = stop_at_ns_.load(std::memory_order_acquire);
            if (stop_at == 0) {
                wake_.wait(seen);
            } else if (Timing::wait_until(stop_at, &wake_, seen)) {
                timed_out_.store(true, std::memory_order_relaxed);
                paused_ = false;
                return Outcome::stop;
            }
        }
    }
}

Player::Outcome Player::wait_to(const int64_t script_time) {
    while (true) {
        const uint32_t seen = wake_.epoch();
        if (requests_.load(std::memory_order_acquire) != 0U) {
            const Outcome outcome = service();
            if (outcome != Outcome::proceed)
                return outcome;
            continue;
        }
        // Re-read every pass: an offset or limit change wakes this wait
        // without a request.
        const int64_t deadline = deadline_for(script_time);
        const int64_t stop_at = stop_at_ns_.load(std::memory_order_acquire);
        if (stop_at != 0 && stop_at <= deadline) {
            if (Timing::wait_until(stop_at, &wake_, seen)) {
                timed_out_.store(true, std::memory_order_relaxed);
                return Outcome::stop;
            }
            continue;
        }
        if (Timing::wait_until(deadline, &wake_, seen))
            return Outcome::proceed;
    }
}

bool Player::finish_segment() {
    flush_now();
    const Segment ending = segment_;
    if (ending == Segment::loop) {
        ++loops_;
        if (sink_ != nullptr)
            sink_->loop_completed(loops_, reports_.load(std::memory_order_relaxed));
        const int64_t limit = loop_limit_.load(std::memory_order_relaxed);
        if (limit > 0 && loops_ >= static_cast<uint64_t>(limit))
            return false;
    }
    // An intro-only script is finished after its intro.
    if (timeline_.rows(Segment::loop) == 0)
        return false;

    // The next segment starts exactly where this one ends in real time, so
    // loops do not drift even when a send overran.
    anchor_real_ += scaled(timeline_.duration(ending) - anchor_script_, anchor_speed_);
    anchor_script_ = 0;
    segment_ = Segment::loop;
    index_ = 0;
    cursor_ = 0;
    publish(PlaybackState::playing, {segment_, 0});
    return true;
}

void Player::run(const PlaybackPosition start) {
    if (!script_ || group_.empty())
        return;

    want_paused_.store(false, std::memory_order_release);
    timed_out_.store(false, std::memory_order_relaxed);
    paused_ = false;
    loops_ = 0;
    reports_.store(0, std::memory_order_relaxed);
    warned_profiles_ = 0;
    rejection_reported_ = false;
    live_.clear();
    staged_keys_.clear();

    const uint32_t missing = script_->required_profiles() & ~group_.profile_mask();
    if (missing != 0U && sink_ != nullptr) {
        sink_->message(Severity::warning, "The script uses the " + describe_profiles(missing) +
                                              ", which is not connected; those rows are skipped.");
        warned_profiles_ = missing;
    }

    jump(start);

    while (true) {
        if (requests_.load(std::memory_order_relaxed) != 0U) {
            const Outcome outcome = service();
            if (outcome == Outcome::stop)
                break;
            if (outcome == Outcome::jumped)
                continue;
        }
        if (group_.active_count() == 0) {
            if (sink_ != nullptr)
                sink_->message(Severity::error,
                               "Every device stopped responding; playback stopped.");
            break;
        }
        // Scripts made only of zero waits never reach wait_to(), so the time
        // limit is also checked per row; one relaxed load when it is unset.
        if (const int64_t stop_at = stop_at_ns_.load(std::memory_order_relaxed);
            stop_at != 0 && Timing::now_ns() >= stop_at) {
            timed_out_.store(true, std::memory_order_relaxed);
            break;
        }
        const std::vector<int64_t>& starts = timeline_.starts(segment_);
        const size_t rows = starts.size() - 1;
        const int64_t due = starts[index_];
        if (due > cursor_) {
            // Everything due earlier goes out as one report before waiting.
            flush_now();
            const Outcome outcome = wait_to(due);
            if (outcome == Outcome::stop)
                break;
            if (outcome == Outcome::jumped)
                continue;
            cursor_ = due;
        }
        if (index_ == rows) {
            if (!finish_segment())
                break;
            continue;
        }
        const std::vector<EventRecord>& records =
            segment_ == Segment::intro ? script_->once_rows : script_->loop_rows;
        execute(records[index_]);
        ++index_;
    }

    flush_now();
    settle(InputState{});
    paused_ = false;
    // Late control requests belong to this run, not the next one.
    requests_.store(0U, std::memory_order_release);
    publish(PlaybackState::stopped, {segment_, 0});
}

} // namespace aoap
