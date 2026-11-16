#include "core/speed_meter.hpp"

namespace dm::core {
    void SpeedMeter::start(TimePoint now, std::uint64_t initial_bytes) noexcept {
        started_ = true;
        anchor_time_ = now;
        anchor_bytes_ = initial_bytes;
        last_speed_ = 0;
    }

    std::uint64_t SpeedMeter::update(std::uint64_t current_bytes, TimePoint now) noexcept {
        if (!started_ || current_bytes < anchor_bytes_) {
            start(now, current_bytes);
            return 0;
        }
        const auto elapsed = now - anchor_time_;

        if (elapsed < std::chrono::seconds(1)) {
            return last_speed_;
        }
        const std::uint64_t delta_bytes = current_bytes - anchor_bytes_;
        const double elapsed_sec = std::chrono::duration<double>(elapsed).count();

        last_speed_ = static_cast<std::uint64_t>(static_cast<double>(delta_bytes) / elapsed_sec);
        anchor_time_ = now;
        anchor_bytes_ = current_bytes;

        return last_speed_;
    }

    std::uint64_t SpeedMeter::current_speed() const noexcept {
        return last_speed_;
    }
}
