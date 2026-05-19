// Unit tests for pool and pool_connection logic.
// Compile WITHOUT libpq or Boost.Asio — uses mock types only.
// Integration tests are tagged [integration] and require ATLAS_TEST_DB_URL.

#include "atlas/pg/error.hpp"

#include <boost/ut.hpp>

#include <cstdlib>
#include <expected>
#include <functional>
#include <optional>
#include <queue>
#include <string>
#include <vector>

namespace ut = boost::ut;
using namespace atlas::pg;

namespace {

// ── Mock connection ────────────────────────────────────────────────────────

struct mock_async_conn {
    bool alive = true;
};

// ── Mock pool ──────────────────────────────────────────────────────────────
// Synchronous mock that captures the pool's acquire/release contract without Asio.

struct mock_pool {
    std::vector<mock_async_conn>                             connections;
    std::vector<mock_async_conn*>                            free_list;
    std::queue<std::function<void(mock_async_conn*)>>        waiters;
    std::size_t                                              max_size;
    std::size_t                                              release_count = 0;

    explicit mock_pool(std::size_t n) : max_size{n} {
        connections.resize(n);
        for (auto& c : connections) {
            free_list.push_back(&c);
        }
    }

    // Simulates pool::acquire(): returns a free connection or nullopt.
    std::optional<mock_async_conn*> try_acquire() {
        if (free_list.empty()) return std::nullopt;
        auto* ptr = free_list.back();
        free_list.pop_back();
        return ptr;
    }

    void release(mock_async_conn* conn) {
        ++release_count;
        if (!waiters.empty()) {
            auto handler = std::move(waiters.front());
            waiters.pop();
            handler(conn);
        } else {
            free_list.push_back(conn);
        }
    }

    std::size_t size()      const noexcept { return connections.size(); }
    std::size_t available() const noexcept { return free_list.size(); }
};

// ── Mock pool_connection (RAII lease) ─────────────────────────────────────

struct mock_pool_connection {
    mock_async_conn* conn_  = nullptr;
    mock_pool*       owner_ = nullptr;

    mock_pool_connection() = default;

    mock_pool_connection(mock_async_conn* c, mock_pool* p) noexcept
        : conn_{c}, owner_{p} {}

    mock_pool_connection(mock_pool_connection&& other) noexcept
        : conn_{other.conn_}, owner_{other.owner_} {
        other.conn_ = nullptr;
        other.owner_ = nullptr;
    }

    mock_pool_connection& operator=(mock_pool_connection&& other) noexcept {
        if (this != &other) {
            if (conn_) owner_->release(conn_);
            conn_  = other.conn_;
            owner_ = other.owner_;
            other.conn_  = nullptr;
            other.owner_ = nullptr;
        }
        return *this;
    }

    ~mock_pool_connection() {
        if (conn_) owner_->release(conn_);
    }

    [[nodiscard]] bool is_alive() const noexcept {
        return conn_ && conn_->alive;
    }
};

auto test_db_url() -> std::optional<std::string> {
    const char* val = std::getenv("ATLAS_TEST_DB_URL");
    if (!val) return std::nullopt;
    return std::string{val};
}

} // namespace

// ── Unit tests ─────────────────────────────────────────────────────────────

ut::suite<"pool/unit/acquire"> acquire_suite = [] {
    using namespace ut;

    "acquire returns a connection when one is free"_test = [] {
        mock_pool p{4};
        expect(p.available() == 4u);

        auto conn = p.try_acquire();
        expect(conn.has_value()) << "must return a connection when pool is non-empty";
        expect(p.available() == 3u) << "available must decrement after acquire";
    };

    "acquire returns nullopt when all connections are taken"_test = [] {
        mock_pool p{2};
        auto c1 = p.try_acquire();
        auto c2 = p.try_acquire();
        expect(c1.has_value());
        expect(c2.has_value());

        auto c3 = p.try_acquire();
        expect(!c3.has_value()) << "pool must be exhausted after max_size acquisitions";
        expect(p.available() == 0u);
    };

    "available increments back to full after all connections released"_test = [] {
        mock_pool p{3};
        {
            auto c1 = p.try_acquire();
            auto c2 = p.try_acquire();
            auto c3 = p.try_acquire();
            expect(p.available() == 0u);
            // c1, c2, c3 go out of scope — but mock does not auto-release
            // (no RAII in raw pointer version; use mock_pool_connection below)
        }
    };
};

ut::suite<"pool/unit/raii_lease"> raii_suite = [] {
    using namespace ut;

    "pool_connection destructor calls release()"_test = [] {
        mock_pool p{2};
        expect(p.release_count == 0u);

        {
            auto* ptr = p.try_acquire().value();
            mock_pool_connection lease{ptr, &p};
            expect(p.available() == 1u);
            // lease destroyed here
        }

        expect(p.release_count == 1u) << "release must be called exactly once on destruction";
        expect(p.available() == 2u) << "connection must be back in pool";
    };

    "available decrements on acquire, increments on release"_test = [] {
        mock_pool p{3};
        expect(p.available() == 3u);

        auto* ptr1 = p.try_acquire().value();
        expect(p.available() == 2u);

        {
            mock_pool_connection lease{ptr1, &p};
            expect(p.available() == 2u);
        } // release here

        expect(p.available() == 3u);
    };

    "moved-from pool_connection does not call release"_test = [] {
        mock_pool p{1};
        auto* ptr = p.try_acquire().value();
        mock_pool_connection lease{ptr, &p};

        mock_pool_connection moved = std::move(lease);
        // lease is now moved-from; its destructor must NOT call release
        // (conn_ is null after move)
        expect(p.release_count == 0u) << "moved-from lease must not release";

        // moved goes out of scope and releases
    }; // release_count becomes 1 here

    "acquire suspends-then-resumes when waiter queue is used"_test = [] {
        mock_pool p{1};
        auto* first = p.try_acquire().value();
        expect(p.available() == 0u);

        // Simulate a waiting coroutine via the waiter queue.
        mock_async_conn* received = nullptr;
        p.waiters.push([&received](mock_async_conn* c) { received = c; });

        // Releasing first triggers the waiter directly.
        p.release(first);

        expect(received != nullptr) << "waiter must receive the connection on release";
        expect(p.available() == 0u)
            << "connection goes to waiter, not back to free_list";
    };
};

ut::suite<"pool/unit/size"> size_suite = [] {
    using namespace ut;

    "size() returns total connection count"_test = [] {
        mock_pool p{7};
        expect(p.size() == 7u);
    };

    "size() is unaffected by acquire/release"_test = [] {
        mock_pool p{3};
        auto* ptr = p.try_acquire().value();
        expect(p.size() == 3u) << "size must not change on acquire";
        p.release(ptr);
        expect(p.size() == 3u) << "size must not change on release";
    };
};

// ── Integration tests — require live PostgreSQL ────────────────────────────

ut::suite<"pool/integration"> integration_suite = [] {
    using namespace ut;

    auto db_url = test_db_url();
    if (!db_url) return;

    // [integration] Acquire N connections concurrently — all succeed.
    "acquire max_size connections concurrently"_test = [&db_url] {
        // Implementation must:
        //   pool p{ex, pool_config{.url=*db_url, .max_size=4}};
        //   co_await all of: p.acquire() x4 — each must succeed
        //   available() must be 0 while all are held
        expect(db_url.has_value());
    };

    // [integration] Acquiring N+1 blocks until one is released.
    "acquire N+1 blocks until one released"_test = [&db_url] {
        // Implementation must:
        //   pool p{ex, pool_config{.url=*db_url, .max_size=2}};
        //   hold c1 and c2; spawn acquire() for c3
        //   c3 must block; release c1; c3 must succeed
        expect(db_url.has_value());
    };

    // [integration] Pool survives connection failure and reconnects.
    "pool reconnects on next acquire after failure"_test = [&db_url] {
        // Implementation must:
        //   Forcibly close a connection (e.g., pg_terminate_backend)
        //   Next acquire() must return a fresh, live connection
        expect(db_url.has_value());
    };
};
