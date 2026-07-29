#pragma once

#include <chrono>

namespace atlas::detail {

[[nodiscard]] constexpr auto next_reconnect_delay(std::chrono::milliseconds current,
                                                  std::chrono::milliseconds maximum) noexcept
    -> std::chrono::milliseconds {
    if (current.count() <= 0 || maximum.count() <= 0) {
        return std::chrono::milliseconds::zero();
    }
    if (current >= maximum || current > maximum - current) {
        return maximum;
    }
    return current + current;
}

} // namespace atlas::detail
