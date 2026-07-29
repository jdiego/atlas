#include "async_test_support.hpp"

#include "async/detail/descriptor_observer.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/ut.hpp>

#include <array>
#include <cerrno>
#include <cstddef>
#include <fcntl.h>
#include <optional>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

namespace ut = boost::ut;
namespace asio = boost::asio;

namespace {

class socket_pair {
public:
    socket_pair() {
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds_.data()) != 0) {
            throw std::runtime_error{"socketpair failed"};
        }
    }

    socket_pair(const socket_pair &) = delete;
    socket_pair &operator=(const socket_pair &) = delete;

    ~socket_pair() {
        for (const int fd : fds_) {
            if (fd >= 0) {
                ::close(fd);
            }
        }
    }

    [[nodiscard]] int local() const noexcept {
        return fds_[0];
    }

    [[nodiscard]] int peer() const noexcept {
        return fds_[1];
    }

    [[nodiscard]] int release_local() noexcept {
        return std::exchange(fds_[0], -1);
    }

private:
    std::array<int, 2> fds_{-1, -1};
};

void fill_send_buffer(int fd) {
    const int flags = ::fcntl(fd, F_GETFL);
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        throw std::runtime_error{"fcntl failed"};
    }

    std::array<std::byte, 4096> bytes{};
    while (::send(fd, bytes.data(), bytes.size(), 0) >= 0) {
    }
    if (errno != EAGAIN && errno != EWOULDBLOCK) {
        throw std::runtime_error{"send failed before the socket became backpressured"};
    }
}

void make_readable(int peer) {
    const std::byte value{};
    if (::send(peer, &value, sizeof(value), 0) != sizeof(value)) {
        throw std::runtime_error{"send to peer failed"};
    }
}

} // namespace

ut::suite<"async/descriptor_observer"> descriptor_observer_suite = [] {
    using namespace ut;

    "read readiness is observed while writes are backpressured"_test = [] {
        asio::io_context ctx;
        socket_pair sockets;
        atlas::detail::descriptor_observer observer{ctx.get_executor()};

        expect(!observer.mirror(sockets.local()));
        fill_send_buffer(sockets.local());
        make_readable(sockets.peer());

        const auto ready = atlas_test::run_on(ctx, observer.wait_read_or_write());
        expect(ready.has_value());
        if (ready) {
            expect(*ready == atlas::detail::descriptor_readiness::read);
        }
    };

    "the same numeric fd is re-registered without taking socket ownership"_test = [] {
        asio::io_context ctx;
        socket_pair original;
        std::optional<socket_pair> replacement;
        int reused_fd = -1;

        {
            atlas::detail::descriptor_observer observer{ctx.get_executor()};
            expect(!observer.mirror(original.local()));

            fill_send_buffer(original.local());
            make_readable(original.peer());
            expect(atlas_test::run_on(ctx, observer.wait_read_or_write()).has_value());

            reused_fd = original.release_local();
            expect(::close(reused_fd) == 0);

            replacement.emplace();
            expect(replacement->local() == reused_fd) << "the OS did not reuse the numeric descriptor";
            expect(!observer.mirror(replacement->local()));

            fill_send_buffer(replacement->local());
            make_readable(replacement->peer());
            ctx.restart();
            const auto ready = atlas_test::run_on(ctx, observer.wait_read_or_write());
            expect(ready.has_value());
            if (ready) {
                expect(*ready == atlas::detail::descriptor_readiness::read);
            }
        }

        expect(::fcntl(replacement->local(), F_GETFD) != -1)
            << "destroying the observer closed the libpq-owned descriptor";
    };
};
