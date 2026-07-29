#include "async/detail/acquire_waiter_queue.hpp"

#include <boost/ut.hpp>

#include <cstddef>
#include <vector>

namespace ut = boost::ut;

ut::suite<"async/acquire_waiter_queue"> acquire_waiter_queue_suite = [] {
    using namespace ut;

    "arbitrary erasure preserves FIFO order"_test = [] {
        atlas::detail::waiter_queue<int> queue;
        std::vector<atlas::detail::waiter_queue<int>::ticket> tickets;
        tickets.reserve(10'000);

        for (int value = 0; value < 10'000; ++value) {
            tickets.push_back(queue.push(value));
        }
        for (std::size_t index = 1; index < tickets.size(); index += 2) {
            queue.erase(tickets[index]);
        }

        expect(queue.size() == 5'000_ul);
        for (int expected = 0; expected < 10'000; expected += 2) {
            expect(queue.pop_front() == expected);
        }
        expect(queue.empty());
    };
};
