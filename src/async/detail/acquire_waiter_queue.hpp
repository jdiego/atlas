#pragma once

#include <cstddef>
#include <list>
#include <utility>

namespace atlas::detail {

template <typename T>
class waiter_queue {
public:
    using value_type = T;
    using storage_type = std::list<value_type>;
    using ticket = storage_type::iterator;

    auto push(value_type waiter) -> ticket {
        return waiters_.insert(waiters_.end(), std::move(waiter));
    }

    void erase(ticket position) {
        waiters_.erase(position);
    }

    auto pop_front() -> value_type {
        auto waiter = std::move(waiters_.front());
        waiters_.pop_front();
        return waiter;
    }

    [[nodiscard]] bool empty() const noexcept {
        return waiters_.empty();
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return waiters_.size();
    }

private:
    storage_type waiters_;
};

} // namespace atlas::detail
