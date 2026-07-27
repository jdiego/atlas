#pragma once

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/posix/stream_descriptor.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/error_code.hpp>

#include <expected>
#include <tuple>
#include <utility>
#include <variant>

namespace atlas::detail {

namespace asio = boost::asio;

enum class descriptor_readiness {
    read,
    write,
};

// Detaches an observed fd without closing it. The underlying socket remains
// exclusively owned by libpq.
inline void detach_descriptor(asio::posix::stream_descriptor &descriptor) noexcept {
    try {
        if (descriptor.is_open()) {
            static_cast<void>(descriptor.release());
        }
    } catch (...) { // NOLINT(bugprone-empty-catch)
    }
}

// Waits until either direction is ready. awaitable_operators cancels and joins
// the losing wait before returning, so no descriptor operation is detached.
[[nodiscard]] inline auto wait_read_or_write(asio::posix::stream_descriptor &descriptor)
    -> asio::awaitable<std::expected<descriptor_readiness, boost::system::error_code>> {
    using namespace asio::experimental::awaitable_operators;

    auto outcome = co_await (
        descriptor.async_wait(asio::posix::stream_descriptor::wait_read, asio::as_tuple(asio::use_awaitable)) ||
        descriptor.async_wait(asio::posix::stream_descriptor::wait_write, asio::as_tuple(asio::use_awaitable)));

    if (outcome.index() == 0) {
        auto [error] = std::get<0>(outcome);
        if (error) {
            co_return std::unexpected(error);
        }
        co_return descriptor_readiness::read;
    }

    auto [error] = std::get<1>(outcome);
    if (error) {
        co_return std::unexpected(error);
    }
    co_return descriptor_readiness::write;
}

// Mirrors a socket owned by libpq. mirror() always drops the old reactor
// registration before assigning, even when the replacement reused the same
// numeric descriptor.
class descriptor_observer {
public:
    explicit descriptor_observer(asio::any_io_executor executor) : descriptor_{std::move(executor)} {
    }

    descriptor_observer(const descriptor_observer &) = delete;
    descriptor_observer &operator=(const descriptor_observer &) = delete;
    descriptor_observer(descriptor_observer &&) = delete;
    descriptor_observer &operator=(descriptor_observer &&) = delete;

    ~descriptor_observer() {
        detach();
    }

    [[nodiscard]] auto mirror(int fd) -> boost::system::error_code {
        detach();
        boost::system::error_code error;
        descriptor_.assign(fd, error);
        return error;
    }

    [[nodiscard]] auto wait(asio::posix::stream_descriptor::wait_type wait_type)
        -> asio::awaitable<boost::system::error_code> {
        auto [error] = co_await descriptor_.async_wait(wait_type, asio::as_tuple(asio::use_awaitable));
        co_return error;
    }

    [[nodiscard]] auto wait_read_or_write()
        -> asio::awaitable<std::expected<descriptor_readiness, boost::system::error_code>> {
        co_return co_await detail::wait_read_or_write(descriptor_);
    }

    void detach() noexcept {
        detach_descriptor(descriptor_);
    }

private:
    asio::posix::stream_descriptor descriptor_;
};

} // namespace atlas::detail
