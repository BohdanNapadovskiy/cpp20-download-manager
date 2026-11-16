#pragma once
#include <chrono>
#include <cstdint>

namespace dm::core {
    class SpeedMeter {
    public:
        using Clock = std::chrono::steady_clock;
        using TimePoint = Clock::time_point;
        void start(TimePoint now, std::uint64_t initial_bytes = 0) noexcept;
        std::uint64_t update(std::uint64_t current_bytes, TimePoint now) noexcept;
        [[nodiscard]] std::uint64_t current_speed() const noexcept;
    private:
        bool started_{false};
        TimePoint anchor_time_{};
        std::uint64_t anchor_bytes_{0};
        std::uint64_t last_speed_{0};
    };
}