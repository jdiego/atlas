// Exercises atlas::pool. The unit suite runs without a server; the integration
// suite needs ATLAS_TEST_CONNINFO.

#include "async_test_support.hpp"

#include "async/detail/pool_test_access.hpp"
#include "atlas/async/pool.hpp"
#include "atlas/async/timeout.hpp"
#include "atlas/pg/connection.hpp"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/system/system_error.hpp>
#include <boost/ut.hpp>
#include <libpq-fe.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <vector>

namespace ut = boost::ut;
namespace asio = boost::asio;
using namespace std::chrono_literals;

namespace {

using atlas::pg::errc;
using atlas_test::conninfo;
using atlas_test::run_on;
using atlas_test::sleep_for;

constexpr std::span<const char *const> no_params{};

struct conninfo_options_deleter {
    void operator()(PQconninfoOption *options) const noexcept {
        if (options != nullptr) {
            PQconninfoFree(options);
        }
    }
};

struct pq_memory_deleter {
    void operator()(char *value) const noexcept {
        if (value != nullptr) {
            PQfreemem(value);
        }
    }
};

[[nodiscard]] auto parsed_option(std::string_view conninfo, std::string_view keyword) -> std::optional<std::string> {
    const std::string copied{conninfo};
    char *raw_error = nullptr;
    std::unique_ptr<char, pq_memory_deleter> error;
    std::unique_ptr<PQconninfoOption, conninfo_options_deleter> options{PQconninfoParse(copied.c_str(), &raw_error)};
    error.reset(raw_error);
    if (!options) {
        return std::nullopt;
    }

    for (auto *option = options.get(); option->keyword != nullptr; ++option) {
        if (keyword == option->keyword && option->val != nullptr) {
            return std::string{option->val};
        }
    }
    return std::nullopt;
}

[[nodiscard]] auto config_for(std::string url, std::size_t max_size, std::chrono::milliseconds timeout)
    -> atlas::pool_config {
    atlas::pool_config cfg;
    cfg.url = std::move(url);
    cfg.max_size = max_size;
    cfg.timeout = timeout;
    return cfg;
}

[[nodiscard]] bool has_one_sslmode_option(std::string_view url) {
    const auto first = url.find("sslmode=");
    return first != std::string_view::npos && url.find("sslmode=", first + 1) == std::string_view::npos;
}

[[nodiscard]] auto with_database(std::string connection_info, std::string_view database) -> std::string {
    if (connection_info.starts_with("postgresql://") || connection_info.starts_with("postgres://")) {
        const auto scheme = connection_info.find("://") + 3;
        const auto query = connection_info.find('?', scheme);
        const auto slash = connection_info.find('/', scheme);
        const auto end = query == std::string::npos ? connection_info.size() : query;
        if (slash == std::string::npos || slash > end) {
            connection_info.insert(end, "/" + std::string{database});
        } else {
            connection_info.replace(slash + 1, end - slash - 1, std::string{database});
        }
        return connection_info;
    }
    connection_info.append(" dbname=").append(database);
    return connection_info;
}

[[nodiscard]] auto recovery_database_name(std::string_view suffix) -> std::string {
    return "atlas_recovery_" + std::to_string(::getpid()) + "_" + std::string{suffix};
}

[[nodiscard]] auto quote_identifier(std::string_view identifier) -> std::string {
    return "\"" + std::string{identifier} + "\"";
}

struct canceled_acquire_outcome {
    bool completed = false;
    bool operation_aborted = false;
    std::exception_ptr failure;
};

[[nodiscard]] auto cancel_on_waiter_enqueue(asio::io_context &ctx, atlas::pool &db) -> canceled_acquire_outcome {
    boost::asio::cancellation_signal signal;
    canceled_acquire_outcome outcome;
    ctx.restart();
    static_cast<void>(run_on(ctx, [&]() -> asio::awaitable<bool> {
        co_await atlas::detail::pool_test_access::set_waiter_enqueued_hook(
            db, [&signal] { signal.emit(asio::cancellation_type::all); });
        co_return true;
    }()));

    auto pending_acquire = [&db]() -> asio::awaitable<void> { static_cast<void>(co_await db.acquire()); };
    asio::co_spawn(ctx, pending_acquire(),
                   asio::bind_cancellation_slot(signal.slot(), [&outcome](std::exception_ptr failure) {
                       outcome.failure = failure;
                       if (failure != nullptr) {
                           try {
                               std::rethrow_exception(failure);
                           } catch (const boost::system::system_error &error) {
                               outcome.operation_aborted = error.code() == asio::error::operation_aborted;
                           } catch (...) {
                           }
                       }
                       outcome.completed = true;
                   }));

    ctx.restart();
    ctx.run();
    return outcome;
}

} // namespace

ut::suite<"async/pool/unit"> pool_unit_suite = [] {
    using namespace ut;

    "sslmode is appended to a connection string"_test = [] {
        const auto url = atlas::apply_ssl_mode("host=localhost dbname=atlas", atlas::ssl_mode::require);
        expect(url.has_value() >> fatal);
        expect(*url == "host=localhost dbname=atlas sslmode=require");
        expect(has_one_sslmode_option(*url));
    };

    "sslmode text inside a keyword value does not suppress the configured mode"_test = [] {
        const auto url =
            atlas::apply_ssl_mode("host=localhost application_name='sslmode=require'", atlas::ssl_mode::disable);
        expect(url.has_value() >> fatal);
        expect(*url == "host=localhost application_name='sslmode=require' sslmode=disable");
    };

    "an explicit URI sslmode is preserved"_test = [] {
        const auto url =
            atlas::apply_ssl_mode("postgresql://localhost/atlas?sslmode=require", atlas::ssl_mode::disable);
        expect(url.has_value() >> fatal);
        expect(*url == "postgresql://localhost/atlas?sslmode=require");
        expect(has_one_sslmode_option(*url));
    };

    "an explicit keyword sslmode is preserved byte for byte"_test = [] {
        const std::string original = "host=localhost sslmode=verify-full application_name='atlas test'";
        const auto url = atlas::apply_ssl_mode(original, atlas::ssl_mode::disable);
        expect(url.has_value() >> fatal);
        expect(*url == original);
        expect(has_one_sslmode_option(*url));
    };

    "sslmode is appended to a URI without a query"_test = [] {
        const auto url = atlas::apply_ssl_mode("postgresql://localhost/atlas", atlas::ssl_mode::require);
        expect(url.has_value() >> fatal);
        expect(parsed_option(*url, "sslmode") == std::optional<std::string>{"require"});
    };

    "sslmode is appended after an unrelated URI query"_test = [] {
        const auto url =
            atlas::apply_ssl_mode("postgresql://localhost/atlas?application_name=atlas", atlas::ssl_mode::disable);
        expect(url.has_value() >> fatal);
        expect(parsed_option(*url, "application_name") == std::optional<std::string>{"atlas"});
        expect(parsed_option(*url, "sslmode") == std::optional<std::string>{"disable"});
    };

    "a raw question mark in a URI password survives TLS injection"_test = [] {
        const auto url = atlas::apply_ssl_mode("postgresql://alice:pa?ss@localhost/atlas", atlas::ssl_mode::require);
        expect(url.has_value() >> fatal);
        expect(parsed_option(*url, "user") == std::optional<std::string>{"alice"});
        expect(parsed_option(*url, "password") == std::optional<std::string>{"pa?ss"});
        expect(parsed_option(*url, "sslmode") == std::optional<std::string>{"require"});
    };

    "a trailing bare URI query marker survives TLS injection"_test = [] {
        const auto url = atlas::apply_ssl_mode("postgresql://localhost/atlas?", atlas::ssl_mode::disable);
        expect(url.has_value() >> fatal);
        expect(parsed_option(*url, "dbname") == std::optional<std::string>{"atlas"});
        expect(parsed_option(*url, "sslmode") == std::optional<std::string>{"disable"});
    };

    "canonical URI conversion escapes quotes and backslashes"_test = [] {
        const auto url =
            atlas::apply_ssl_mode("postgresql://alice:p%27a%5Css@localhost/atlas", atlas::ssl_mode::verify_full);
        expect(url.has_value() >> fatal);
        expect(url->find("password='p\\'a\\\\ss'") != std::string::npos)
            << "the canonical keyword value was not escaped";
        expect(parsed_option(*url, "password") == std::optional<std::string>{"p'a\\ss"});
        expect(parsed_option(*url, "sslmode") == std::optional<std::string>{"verify-full"});
    };

    "only exact libpq sslmode values are accepted"_test = [] {
        for (const std::string_view mode : {"disable", "allow", "prefer", "require", "verify-ca", "verify-full"}) {
            const auto url =
                atlas::apply_ssl_mode("host=localhost sslmode=" + std::string{mode}, atlas::ssl_mode::prefer);
            expect(url.has_value()) << mode;
            if (url) {
                expect(parsed_option(*url, "sslmode") == std::optional<std::string>{mode});
            }
        }

        for (const std::string_view mode : {"", "Require", "verify_ca", "invalid"}) {
            const auto url =
                atlas::apply_ssl_mode("host=localhost sslmode='" + std::string{mode} + "'", atlas::ssl_mode::prefer);
            expect(!url.has_value()) << mode;
            if (!url) {
                expect(url.error().code == errc::invalid_argument);
            }
        }
    };

    "ports use numeric syntax and support multi-host lists"_test = [] {
        const auto multi = atlas::apply_ssl_mode("host=one,two,three port=5432,,5434", atlas::ssl_mode::disable);
        expect(multi.has_value());

        for (const std::string_view port : {"54x2", "5432,other", "5432, 5433", "-1"}) {
            const auto url =
                atlas::apply_ssl_mode("host=localhost port='" + std::string{port} + "'", atlas::ssl_mode::prefer);
            expect(!url.has_value()) << port;
            if (!url) {
                expect(url.error().code == errc::invalid_argument);
            }
        }
    };

    "an embedded NUL in conninfo is rejected"_test = [] {
        using namespace std::string_literals;

        const auto url = atlas::apply_ssl_mode(std::string{"host=local\0host", 15}, atlas::ssl_mode::prefer);
        expect(!url.has_value());
        if (url) {
            return;
        }
        expect(url.error().code == errc::invalid_argument);
    };

    "malformed conninfo is rejected"_test = [] {
        const auto url = atlas::apply_ssl_mode("host='unterminated", atlas::ssl_mode::prefer);
        expect(!url.has_value());
        if (url) {
            return;
        }
        expect(url.error().code == errc::invalid_argument);
    };

    "invalid pool config is a cached terminal acquire error"_test = [] {
        asio::io_context ctx;
        auto cfg = config_for("host='unterminated", 1, 5s);
        atlas::pool db{ctx.get_executor(), cfg};

        const bool rejected = run_on(ctx, [&]() -> asio::awaitable<bool> {
            std::optional<std::string> parse_message;
            for (int request = 0; request < 3; ++request) {
                auto acquired = co_await db.acquire();
                expect(!acquired.has_value());
                if (acquired) {
                    co_return false;
                }
                expect(acquired.error().code == errc::invalid_argument);
                if (!parse_message) {
                    parse_message = acquired.error().message;
                } else {
                    expect(acquired.error().message == *parse_message)
                        << "acquire did not return the cached terminal parse error";
                }
            }
            co_return true;
        }());

        expect(rejected);
    };

    "invalid sslmode and port are cached without recovery"_test = [] {
        for (const std::string_view invalid : {"host=localhost sslmode=invalid", "host=localhost port=not-a-port"}) {
            asio::io_context ctx;
            atlas::pool db{ctx.get_executor(), config_for(std::string{invalid}, 1, 5s)};

            const bool rejected = run_on(ctx, [&]() -> asio::awaitable<bool> {
                for (int request = 0; request < 2; ++request) {
                    auto acquired = co_await db.acquire();
                    expect(!acquired.has_value());
                    if (acquired) {
                        co_return false;
                    }
                    expect(acquired.error().code == errc::invalid_argument);
                }

                const auto snapshot = co_await atlas::detail::pool_test_access::recovery_snapshot(db);
                expect(snapshot.connect_attempt_count == 0_ul);
                expect(snapshot.missing_loop_count == 0_ul);
                expect(snapshot.timer_count == 0_ul);
                co_return true;
            }());

            expect(rejected);
        }
    };

    "pending intrinsic cancellation erases a queued ticket"_test = [] {
        asio::io_context pool_ctx;
        atlas::pool db{pool_ctx.get_executor(), config_for("host=localhost", 0, 100ms)};

        const auto outcome = cancel_on_waiter_enqueue(pool_ctx, db);
        expect(outcome.completed);
        expect(outcome.failure != nullptr);
        expect(outcome.operation_aborted);

        pool_ctx.restart();
        const auto queued = run_on(pool_ctx, [&]() -> asio::awaitable<std::size_t> {
            co_return co_await atlas::detail::pool_test_access::waiter_count(db);
        }());
        expect(queued == 0_ul) << "intrinsic cancellation orphaned a queued acquire";
    };

    "an unreachable server yields a failure, not a hang"_test = [] {
        asio::io_context ctx;
        auto db =
            std::make_unique<atlas::pool>(ctx.get_executor(), config_for(atlas_test::unreachable_conninfo, 1, 5s));

        // pool_connection has no default constructor, so the lease cannot cross
        // co_spawn's completion handler; report the error code instead.
        const auto code = run_on(ctx, [&]() -> asio::awaitable<std::optional<errc>> {
            auto res = co_await db->acquire();
            expect(db->size() == 0_ul);
            db.reset(); // perpetual recovery ends only when the pool shuts down
            if (res) {
                co_return std::nullopt;
            }
            co_return res.error().code;
        }());

        expect(code.has_value()) << "a lease was handed out with no server behind the pool";
        if (code) {
            expect(*code == errc::connection_failure);
        }
    };

    // Regression: initialisation used to be co_spawned with a raw `this`, so a
    // pool destroyed mid-initialisation left the coroutine on freed memory.
    "destroying the pool during initialisation is safe"_test = [] {
        asio::io_context ctx;

        {
            atlas::pool db{ctx.get_executor(), config_for(atlas_test::unreachable_conninfo, 4, 5s)};
            ctx.poll(); // let initialise() start and suspend on the first connect
        }

        ctx.run(); // the detached coroutine finishes after the pool is gone
        expect(true);
    };

    "destroying the pool cancels recovery backoff and wakes a later acquire"_test = [] {
        asio::io_context ctx;
        auto cfg = config_for(atlas_test::unreachable_conninfo, 1, 10s);
        cfg.max_retries = 3;
        cfg.reconnect_initial_delay = 5s;
        cfg.reconnect_max_delay = 5s;
        auto db = std::make_unique<atlas::pool>(ctx.get_executor(), cfg);

        const auto run_started = std::chrono::steady_clock::now();
        const bool ran = run_on(ctx, [&]() -> asio::awaitable<bool> {
            // Initialisation was spawned first and may win the strand race. The
            // zero-open pass still owes this first caller exactly one failure.
            auto initial = co_await db->acquire();
            expect(!initial.has_value());
            if (initial) {
                co_return false;
            }
            expect(initial.error().code == errc::connection_failure);

            // One initial attempt plus one three-attempt recovery cycle must
            // produce exactly one registered backoff timer for this slot.
            std::optional<atlas::detail::pool_recovery_snapshot> snapshot;
            const auto snapshot_deadline = std::chrono::steady_clock::now() + 1s;
            while (std::chrono::steady_clock::now() < snapshot_deadline) {
                auto current = co_await atlas::detail::pool_test_access::recovery_snapshot(*db);
                if (current.timer_count == 1) {
                    snapshot = current;
                    break;
                }
                co_await sleep_for(5ms);
            }
            expect(snapshot.has_value()) << "the missing slot never entered recovery backoff";
            if (snapshot) {
                expect(snapshot->timer_count == 1_ul) << "one missing slot created duplicate recovery loops";
                expect(snapshot->missing_loop_count == 1_ul) << "one missing slot spawned duplicate recovery loops";
                expect(snapshot->connect_attempt_count == 4_ul) << "the recovery cycle did not honor max_retries=3";
            }

            // Await directly from this frame. The timer callback resets the
            // pool while acquire() is suspended, so no detached coroutine can
            // retain references into a completed parent frame.
            asio::steady_timer shutdown_timer{co_await asio::this_coro::executor};
            shutdown_timer.expires_after(50ms);
            auto shutdown_happened = std::make_shared<bool>(false);
            shutdown_timer.async_wait([&db, shutdown_happened](const boost::system::error_code &ec) {
                if (!ec) {
                    *shutdown_happened = true;
                    db.reset();
                }
            });

            const auto shutdown_at = std::chrono::steady_clock::now();
            auto later = co_await db->acquire();
            const auto shutdown_time = std::chrono::steady_clock::now() - shutdown_at;

            // Keep the negative path finite too: if acquire completes before
            // the timer callback, destroy the perpetual-recovery pool here.
            db.reset();

            expect(*shutdown_happened) << "the later acquire completed before pool shutdown";
            expect(shutdown_time < 1s) << "shutdown waited for the five-second reconnect delay";
            expect(!later.has_value());
            if (!later) {
                expect(later.error().code == errc::connection_failure);
            }
            co_return snapshot.has_value() && *shutdown_happened && !later &&
                later.error().code == errc::connection_failure;
        }());
        const auto run_time = std::chrono::steady_clock::now() - run_started;

        expect(run_time < 1s) << "the uncancelled recovery timer kept the executor alive";
        expect(ran);
    };
};

ut::suite<"async/pool/integration"> pool_integration_suite = [] {
    using namespace ut;

    const auto url = conninfo();
    if (!url) {
        // Reported rather than silently contributing zero tests, so a CI run
        // without a server is visibly uncovered instead of looking green.
        skip / "requires a live server via ATLAS_TEST_CONNINFO"_test = [] {};
        return;
    }

    "pool recovers a database unavailable during initialisation"_test = [&url] {
        const std::string database = recovery_database_name("initial");
        const std::string quoted_name = quote_identifier(database);
        auto admin = atlas::pg::connection::connect(*url);
        expect(admin.has_value() >> fatal);
        if (!admin) {
            return;
        }

        auto absent = admin->exec("DROP DATABASE IF EXISTS " + quoted_name + " WITH (FORCE)");
        expect(absent.has_value() >> fatal);
        if (!absent) {
            return;
        }

        asio::io_context ctx;
        auto cfg = config_for(with_database(*url, database), 3, 5s);
        cfg.max_retries = 1;
        cfg.reconnect_initial_delay = 5s;
        cfg.reconnect_max_delay = 5s;
        auto recovering_pool = std::make_unique<atlas::pool>(ctx.get_executor(), cfg);

        const bool ran = run_on(ctx, [&]() -> asio::awaitable<bool> {
            bool succeeded = true;

            // Whether queued during initialisation or scheduled just after it,
            // the first caller observes the zero-open pass's last error.
            auto first = co_await recovering_pool->acquire();
            expect(!first.has_value());
            if (first) {
                succeeded = false;
            } else {
                expect(first.error().code == errc::connection_failure);
            }

            std::optional<atlas::detail::pool_recovery_snapshot> snapshot;
            if (succeeded) {
                const auto snapshot_deadline = std::chrono::steady_clock::now() + 1s;
                while (std::chrono::steady_clock::now() < snapshot_deadline) {
                    auto current = co_await atlas::detail::pool_test_access::recovery_snapshot(*recovering_pool);
                    if (current.timer_count == 3) {
                        snapshot = current;
                        break;
                    }
                    co_await sleep_for(5ms);
                }
                expect(snapshot.has_value()) << "not every missing slot entered one recovery backoff";
                if (snapshot) {
                    expect(snapshot->timer_count == 3_ul) << "missing slots created duplicate recovery loops";
                    expect(snapshot->missing_loop_count == 3_ul) << "unexpected number of missing-slot recovery loops";
                    expect(snapshot->connect_attempt_count == 6_ul)
                        << "unexpected connection attempts before the first backoff";
                } else {
                    succeeded = false;
                }
            }

            if (succeeded) {
                auto created = admin->exec("CREATE DATABASE " + quoted_name);
                expect(created.has_value());
                if (!created) {
                    succeeded = false;
                }
            }

            if (succeeded) {
                const bool recovered = co_await atlas_test::wait_until(
                    [&recovering_pool] { return recovering_pool->size() == 3 && recovering_pool->available() == 3; },
                    10s, 20ms);
                expect(recovered) << "the existing pool did not recover every missing slot";
                if (!recovered) {
                    succeeded = false;
                }
            }

            if (succeeded) {
                auto result = co_await recovering_pool->execute("SELECT 1", no_params);
                expect(result.has_value()) << (result ? "" : result.error().message);
                succeeded = result.has_value();
            }

            // Recovery timers are perpetual on failure, so shutdown must happen
            // on every branch before run_on() waits for executor exhaustion.
            recovering_pool.reset();
            co_return succeeded;
        }());

        auto cleaned = admin->exec("DROP DATABASE IF EXISTS " + quoted_name + " WITH (FORCE)");
        expect(cleaned.has_value()) << (cleaned ? "" : cleaned.error().message);
        expect(ran);
    };

    "an unavailable existing slot recovers after its database returns"_test = [&url] {
        const std::string database = recovery_database_name("existing");
        const std::string quoted_name = quote_identifier(database);
        auto admin = atlas::pg::connection::connect(*url);
        expect(admin.has_value() >> fatal);
        if (!admin) {
            return;
        }

        expect(admin->exec("DROP DATABASE IF EXISTS " + quoted_name + " WITH (FORCE)").has_value() >> fatal);
        expect(admin->exec("CREATE DATABASE " + quoted_name).has_value() >> fatal);

        asio::io_context ctx;
        auto cfg = config_for(with_database(*url, database), 1, 5s);
        cfg.max_retries = 1;
        cfg.reconnect_initial_delay = 100ms;
        cfg.reconnect_max_delay = 100ms;
        auto recovering_pool = std::make_unique<atlas::pool>(ctx.get_executor(), cfg);

        const bool ran = run_on(ctx, [&]() -> asio::awaitable<bool> {
            bool succeeded = true;
            auto acquired = co_await recovering_pool->acquire();
            expect(acquired.has_value());
            std::optional<atlas::pool_connection> held;
            if (acquired) {
                held.emplace(std::move(*acquired));
            } else {
                succeeded = false;
            }

            if (succeeded) {
                auto dropped = admin->exec("DROP DATABASE " + quoted_name + " WITH (FORCE)");
                expect(dropped.has_value());
                if (!dropped) {
                    succeeded = false;
                }
            }

            if (succeeded) {
                auto failed = co_await held->execute("SELECT 1", no_params);
                expect(!failed.has_value()) << "the force-dropped backend still served a query";
                succeeded = !failed.has_value();
            }
            held.reset();

            if (succeeded) {
                const bool unavailable =
                    co_await atlas_test::wait_until([&recovering_pool] { return recovering_pool->size() == 0; }, 1s);
                expect(unavailable) << "the dead existing slot remained in the usable size";
                succeeded = unavailable;
            }

            if (succeeded) {
                std::optional<atlas::detail::pool_recovery_snapshot> snapshot;
                const auto snapshot_deadline = std::chrono::steady_clock::now() + 1s;
                while (std::chrono::steady_clock::now() < snapshot_deadline) {
                    auto current = co_await atlas::detail::pool_test_access::recovery_snapshot(*recovering_pool);
                    if (current.timer_count == 1) {
                        snapshot = current;
                        break;
                    }
                    co_await sleep_for(5ms);
                }
                expect(snapshot.has_value()) << "the existing slot did not retry while its database was absent";
                succeeded = snapshot.has_value();
            }

            if (succeeded) {
                auto created = admin->exec("CREATE DATABASE " + quoted_name);
                expect(created.has_value());
                if (!created) {
                    succeeded = false;
                }
            }

            if (succeeded) {
                const bool recovered = co_await atlas_test::wait_until(
                    [&recovering_pool] { return recovering_pool->size() == 1 && recovering_pool->available() == 1; },
                    5s, 20ms);
                expect(recovered) << "the unavailable existing slot was permanently retired";
                succeeded = recovered;
            }

            if (succeeded) {
                auto result = co_await recovering_pool->execute("SELECT 1", no_params);
                expect(result.has_value()) << (result ? "" : result.error().message);
                succeeded = result.has_value();
            }

            recovering_pool.reset();
            co_return succeeded;
        }());

        auto cleaned = admin->exec("DROP DATABASE IF EXISTS " + quoted_name + " WITH (FORCE)");
        expect(cleaned.has_value()) << (cleaned ? "" : cleaned.error().message);
        expect(ran);
    };

    "a recovered missing slot is handed directly to its queued waiter"_test = [&url] {
        const std::string database = recovery_database_name("waiter");
        const std::string quoted_name = quote_identifier(database);
        auto admin = atlas::pg::connection::connect(*url);
        expect(admin.has_value() >> fatal);
        if (!admin) {
            return;
        }
        expect(admin->exec("DROP DATABASE IF EXISTS " + quoted_name + " WITH (FORCE)").has_value() >> fatal);

        asio::io_context ctx;
        auto cfg = config_for(with_database(*url, database), 1, 5s);
        cfg.max_retries = 1;
        cfg.reconnect_initial_delay = 100ms;
        cfg.reconnect_max_delay = 100ms;
        auto recovering_pool = std::make_unique<atlas::pool>(ctx.get_executor(), cfg);

        struct create_outcome {
            bool attempted = false;
            bool succeeded = false;
        };
        auto creation = std::make_shared<create_outcome>();

        const bool ran = run_on(ctx, [&]() -> asio::awaitable<bool> {
            bool succeeded = true;
            auto first = co_await recovering_pool->acquire();
            expect(!first.has_value());
            if (first) {
                succeeded = false;
            }

            if (succeeded) {
                bool in_backoff = false;
                const auto backoff_deadline = std::chrono::steady_clock::now() + 1s;
                while (std::chrono::steady_clock::now() < backoff_deadline) {
                    auto snapshot = co_await atlas::detail::pool_test_access::recovery_snapshot(*recovering_pool);
                    if (snapshot.timer_count == 1) {
                        in_backoff = true;
                        break;
                    }
                    co_await sleep_for(5ms);
                }
                expect(in_backoff);
                succeeded = in_backoff;
            }

            asio::steady_timer create_timer{co_await asio::this_coro::executor};
            create_timer.expires_after(20ms);
            auto *admin_connection = &*admin;
            create_timer.async_wait([creation, admin_connection, quoted_name](const boost::system::error_code &ec) {
                if (!ec) {
                    creation->attempted = true;
                    creation->succeeded = admin_connection->exec("CREATE DATABASE " + quoted_name).has_value();
                }
            });

            auto recovered = co_await recovering_pool->acquire();
            expect(creation->attempted);
            expect(creation->succeeded);
            expect(recovered.has_value()) << (recovered ? "" : recovered.error().message);
            if (!recovered) {
                succeeded = false;
            } else {
                expect(recovering_pool->size() == 1_ul);
                expect(recovering_pool->available() == 0_ul)
                    << "the recovered slot was parked instead of handed to the queued waiter";
                auto result = co_await recovered->execute("SELECT 1", no_params);
                expect(result.has_value()) << (result ? "" : result.error().message);
                succeeded = succeeded && result.has_value() && creation->succeeded && recovering_pool->available() == 0;
            }

            recovering_pool.reset();
            co_return succeeded;
        }());

        auto cleaned = admin->exec("DROP DATABASE IF EXISTS " + quoted_name + " WITH (FORCE)");
        expect(cleaned.has_value()) << (cleaned ? "" : cleaned.error().message);
        expect(ran);
    };

    "acquire hands out a working connection and takes it back"_test = [&url] {
        asio::io_context ctx;
        atlas::pool db{ctx.get_executor(), config_for(*url, 2, 5s)};

        const bool ran = run_on(ctx, [&]() -> asio::awaitable<bool> {
            {
                auto lease = co_await db.acquire();
                expect(lease.has_value()) << (lease ? "" : lease.error().message);
                if (!lease) {
                    co_return false;
                }

                expect(lease->is_alive());
                auto res = co_await lease->execute("SELECT 1", no_params);
                expect(res.has_value()) << (res ? "" : res.error().message);
            }

            // The lease destructor dispatches the release; give the strand a turn.
            co_await sleep_for(20ms);
            expect(db.available() == 2_ul) << "the lease was not returned to the pool";
            co_return true;
        }());

        expect(ran);
    };

    // pool_config is the single place the cleanup budget is set; with_timeout
    // reads it off the lease rather than having every call site pass it.
    "the configured cleanup budget reaches the lease"_test = [&url] {
        asio::io_context ctx;
        auto cfg = config_for(*url, 1, 5s);
        cfg.cleanup_budget = 750ms;
        atlas::pool db{ctx.get_executor(), cfg};

        const bool ran = run_on(ctx, [&]() -> asio::awaitable<bool> {
            auto lease = co_await db.acquire();
            expect(lease.has_value()) << (lease ? "" : lease.error().message);
            if (!lease) {
                co_return false;
            }

            expect(lease->cleanup_budget() == 750ms);
            co_return true;
        }());

        expect(ran);
    };

    "an exhausted cleanup budget invalidates and revives the pooled connection"_test = [&url] {
        asio::io_context ctx;
        auto cfg = config_for(*url, 1, 5s);
        cfg.cleanup_budget = 0ms;
        atlas::pool db{ctx.get_executor(), cfg};

        const bool ran = run_on(ctx, [&]() -> asio::awaitable<bool> {
            {
                auto lease = co_await db.acquire();
                expect(lease.has_value()) << (lease ? "" : lease.error().message);
                if (!lease) {
                    co_return false;
                }

                auto timed_out = co_await atlas::with_timeout<atlas::pg::result>(
                    20ms, *lease, lease->execute("SELECT pg_sleep(10)", no_params));

                expect(!timed_out.has_value());
                expect(timed_out.error().code == errc::query_canceled);
                expect(!lease->is_alive()) << "the lease survived an exhausted cleanup budget";
            }

            auto replacement = co_await db.acquire();
            expect(replacement.has_value()) << (replacement ? "" : replacement.error().message);
            if (!replacement) {
                co_return false;
            }

            auto result = co_await replacement->execute("SELECT 1", no_params);
            expect(result.has_value()) << (result ? "" : result.error().message);
            expect(replacement->is_alive());
            co_return result.has_value();
        }());

        expect(ran);
    };

    "a connection returned in an open transaction is replaced"_test = [&url] {
        asio::io_context ctx;
        atlas::pool db{ctx.get_executor(), config_for(*url, 1, 5s)};

        const bool ran = run_on(ctx, [&]() -> asio::awaitable<bool> {
            auto acquired = co_await db.acquire();
            expect(acquired.has_value() >> fatal);
            if (!acquired) {
                co_return false;
            }

            std::optional<atlas::pool_connection> first{std::move(*acquired)};
            auto pid_result = co_await first->execute("SELECT pg_backend_pid()", no_params);
            expect(pid_result.has_value() >> fatal);
            if (!pid_result) {
                co_return false;
            }

            auto pid_field = pid_result->get(0, 0);
            expect(pid_field.has_value() >> fatal);
            if (!pid_field || !pid_field->has_value()) {
                co_return false;
            }
            const std::string first_pid{pid_field->value()};

            expect((co_await first->execute("BEGIN", no_params)).has_value());
            first.reset();

            auto second = co_await db.acquire();
            expect(second.has_value() >> fatal);
            if (!second) {
                co_return false;
            }

            auto second_pid_result = co_await second->execute("SELECT pg_backend_pid()", no_params);
            expect(second_pid_result.has_value() >> fatal);
            if (!second_pid_result) {
                co_return false;
            }

            auto second_pid_field = second_pid_result->get(0, 0);
            expect(second_pid_field.has_value() >> fatal);
            if (!second_pid_field || !second_pid_field->has_value()) {
                co_return false;
            }

            expect(std::string{second_pid_field->value()} != first_pid)
                << "the pool recirculated a session with an open transaction";
            co_return true;
        }());

        expect(ran);
    };

    "a connection returned in an aborted transaction is replaced"_test = [&url] {
        asio::io_context ctx;
        atlas::pool db{ctx.get_executor(), config_for(*url, 1, 5s)};

        const bool ran = run_on(ctx, [&]() -> asio::awaitable<bool> {
            auto acquired = co_await db.acquire();
            expect(acquired.has_value() >> fatal);
            if (!acquired) {
                co_return false;
            }

            std::optional<atlas::pool_connection> first{std::move(*acquired)};
            auto pid_result = co_await first->execute("SELECT pg_backend_pid()", no_params);
            expect(pid_result.has_value() >> fatal);
            if (!pid_result) {
                co_return false;
            }

            auto pid_field = pid_result->get(0, 0);
            expect(pid_field.has_value() >> fatal);
            if (!pid_field || !pid_field->has_value()) {
                co_return false;
            }
            const std::string first_pid{pid_field->value()};

            expect((co_await first->execute("BEGIN", no_params)).has_value());
            expect(!(co_await first->execute("SELECT 1 / 0", no_params)).has_value());
            first.reset();

            auto second = co_await db.acquire();
            expect(second.has_value() >> fatal);
            if (!second) {
                co_return false;
            }

            auto second_pid_result = co_await second->execute("SELECT pg_backend_pid()", no_params);
            expect(second_pid_result.has_value() >> fatal);
            if (!second_pid_result) {
                co_return false;
            }

            auto second_pid_field = second_pid_result->get(0, 0);
            expect(second_pid_field.has_value() >> fatal);
            if (!second_pid_field || !second_pid_field->has_value()) {
                co_return false;
            }

            expect(std::string{second_pid_field->value()} != first_pid)
                << "the pool recirculated a session with an aborted transaction";
            co_return true;
        }());

        expect(ran);
    };

    "an unsupported COPY invalidates and revives the pooled connection"_test = [&url] {
        asio::io_context ctx;
        atlas::pool db{ctx.get_executor(), config_for(*url, 1, 5s)};

        const bool ran = run_on(ctx, [&]() -> asio::awaitable<bool> {
            {
                auto lease = co_await db.acquire();
                expect(lease.has_value()) << (lease ? "" : lease.error().message);
                if (!lease) {
                    co_return false;
                }

                auto sent = lease->send_query("COPY (SELECT 1) TO STDOUT", no_params);
                expect(sent.has_value()) << (sent ? "" : sent.error().message);

                auto copy = co_await lease->receive();
                expect(!copy.has_value()) << "COPY was exposed as an ordinary query result";
                if (!copy) {
                    expect(copy.error().code == errc::invalid_state);
                }
                expect(!lease->is_alive()) << "a connection left in COPY mode remained reusable";
            }

            auto replacement = co_await db.acquire();
            expect(replacement.has_value()) << (replacement ? "" : replacement.error().message);
            if (!replacement) {
                co_return false;
            }

            auto result = co_await replacement->execute("SELECT 1", no_params);
            expect(result.has_value()) << (result ? "" : result.error().message);
            co_return result.has_value();
        }());

        expect(ran);
    };

    "a connection returned with an unfinished result stream is replaced"_test = [&url] {
        asio::io_context ctx;
        atlas::pool db{ctx.get_executor(), config_for(*url, 1, 5s)};

        const bool ran = run_on(ctx, [&]() -> asio::awaitable<bool> {
            auto acquired = co_await db.acquire();
            expect(acquired.has_value() >> fatal);
            if (!acquired) {
                co_return false;
            }

            std::optional<atlas::pool_connection> first{std::move(*acquired)};
            auto pid_result = co_await first->execute("SELECT pg_backend_pid()", no_params);
            expect(pid_result.has_value() >> fatal);
            if (!pid_result) {
                co_return false;
            }

            auto pid_field = pid_result->get(0, 0);
            expect(pid_field.has_value() >> fatal);
            if (!pid_field || !pid_field->has_value()) {
                co_return false;
            }
            const std::string first_pid{pid_field->value()};

            expect(first->send_query("SELECT 1", no_params).has_value());
            auto one = co_await first->receive();
            expect(one.has_value());
            first.reset();

            auto second = co_await db.acquire();
            expect(second.has_value() >> fatal);
            if (!second) {
                co_return false;
            }

            auto second_pid_result = co_await second->execute("SELECT pg_backend_pid()", no_params);
            expect(second_pid_result.has_value() >> fatal);
            if (!second_pid_result) {
                co_return false;
            }

            auto second_pid_field = second_pid_result->get(0, 0);
            expect(second_pid_field.has_value() >> fatal);
            if (!second_pid_field || !second_pid_field->has_value()) {
                co_return false;
            }

            expect(std::string{second_pid_field->value()} != first_pid)
                << "the pool recirculated a session with unread results";
            co_return true;
        }());

        expect(ran);
    };

    "pool::execute leases and releases around the query"_test = [&url] {
        asio::io_context ctx;
        atlas::pool db{ctx.get_executor(), config_for(*url, 1, 5s)};

        const bool ran = run_on(ctx, [&]() -> asio::awaitable<bool> {
            auto res = co_await db.execute("SELECT 1", no_params);
            expect(res.has_value()) << (res ? "" : res.error().message);

            co_await sleep_for(20ms);
            expect(db.available() == 1_ul);
            co_return true;
        }());

        expect(ran);
    };

    "a request beyond capacity waits for a release"_test = [&url] {
        asio::io_context ctx;
        atlas::pool db{ctx.get_executor(), config_for(*url, 1, 5s)};

        const bool ran = run_on(ctx, [&]() -> asio::awaitable<bool> {
            auto first = co_await db.acquire();
            expect(first.has_value()) << (first ? "" : first.error().message);
            if (!first) {
                co_return false;
            }

            std::optional<atlas::pool_connection> held;
            held.emplace(std::move(*first));

            bool second_served = false;

            // The closure must be a named local: a lambda coroutine reaches its
            // captures through the closure object, and a temporary one would be
            // gone by the time the coroutine first resumes.
            auto queued_request = [&]() -> asio::awaitable<void> {
                auto second = co_await db.acquire();
                expect(second.has_value()) << "the queued waiter was never served";
                second_served = second.has_value();
            };
            asio::co_spawn(co_await asio::this_coro::executor, queued_request(), asio::detached);

            co_await sleep_for(50ms);
            expect(!second_served) << "the second request did not wait for capacity";

            held.reset(); // releases the connection, waking the waiter
            co_await sleep_for(50ms);
            expect(second_served);
            co_return true;
        }());

        expect(ran);
    };

    // Regression: release() used to put any connection straight back into
    // circulation. A connection the server has dropped would then be handed to
    // the next waiter, fail their query, and come back again — one network drop
    // permanently costing the pool a slot.
    "a connection killed server-side is replaced, not recirculated"_test = [&url] {
        asio::io_context ctx;
        auto cfg = config_for(*url, 1, 5s);
        cfg.cleanup_budget = 250ms; // must survive the reconnect below
        atlas::pool db{ctx.get_executor(), cfg};

        const bool ran = run_on(ctx, [&]() -> asio::awaitable<bool> {
            {
                auto lease = co_await db.acquire();
                expect(lease.has_value()) << (lease ? "" : lease.error().message);
                if (!lease) {
                    co_return false;
                }

                // The backend terminates itself, so the lease is dead by the
                // time its destructor hands it back.
                static_cast<void>(co_await lease->execute("SELECT pg_terminate_backend(pg_backend_pid())", no_params));
                expect(!lease->is_alive()) << "the connection survived its own backend being terminated";
            }

            const bool revived = co_await atlas_test::wait_until([&db] { return db.available() == 1; }, 5s, 10ms);
            expect(revived) << "the pool did not publish a replacement connection";
            if (!revived) {
                co_return false;
            }

            expect(db.size() == 1_ul) << "the slot was retired instead of reconnected";

            auto reused = co_await db.execute("SELECT 1", no_params);
            expect(reused.has_value()) << "the pool never recovered a usable connection";

            auto replacement = co_await db.acquire();
            expect(replacement.has_value());
            if (replacement) {
                expect(replacement->cleanup_budget() == 250ms) << "the replacement lost the configured budget";
            }
            co_return true;
        }());

        expect(ran);
    };

    // Drives the pool from several threads while size()/available() are read
    // from another. Those observers used to read vector::size() straight off
    // the containers the strand mutates, which is undefined behaviour rather
    // than a benign race. Run this under TSan for it to mean anything.
    "concurrent use and observation stay race-free"_test = [&url] {
        // Fewer workers than connections on purpose: a saturated pool hands
        // connections straight from one waiter to the next and never touches the
        // free list, so the observers would have nothing to race against.
        constexpr int pool_size = 4;
        constexpr int worker_count = 3;
        constexpr int thread_count = 4;

        asio::io_context ctx;
        atlas::pool db{ctx.get_executor(), config_for(*url, pool_size, 10s)};

        std::atomic<int> completed{0};

        // Each worker churns the free list for a while, so the polling loop
        // below overlaps with a steady stream of acquire/release rather than a
        // burst that is over in microseconds.
        constexpr int queries_per_worker = 200;

        auto worker = [&]() -> asio::awaitable<void> {
            for (int i = 0; i < queries_per_worker; ++i) {
                auto res = co_await db.execute("SELECT 1", no_params);
                if (!res) {
                    co_return;
                }
            }
            completed.fetch_add(1, std::memory_order_relaxed);
        };
        for (int i = 0; i < worker_count; ++i) {
            asio::co_spawn(ctx, worker(), asio::detached);
        }

        std::vector<std::thread> threads;
        threads.reserve(thread_count);
        for (int i = 0; i < thread_count; ++i) {
            threads.emplace_back([&ctx] { ctx.run(); });
        }

        // Poll the observers while the strand hands connections around. The loop
        // deliberately touches no atomic: reading `completed` here would hand
        // ThreadSanitizer a happens-before edge and hide the very race this
        // test exists to catch.
        const auto until = std::chrono::steady_clock::now() + 300ms;
        std::size_t observations = 0;
        while (std::chrono::steady_clock::now() < until) {
            observations += db.size() + db.available();
        }

        for (auto &thread : threads) {
            thread.join();
        }

        expect(completed.load() == _i(worker_count)) << "not every concurrent query completed";
        expect(observations > 0_ul) << "the observers were never polled";
    };

    "acquire times out while every connection is leased"_test = [&url] {
        asio::io_context ctx;
        atlas::pool db{ctx.get_executor(), config_for(*url, 1, 100ms)};

        const bool ran = run_on(ctx, [&]() -> asio::awaitable<bool> {
            auto first = co_await db.acquire();
            expect(first.has_value()) << (first ? "" : first.error().message);
            if (!first) {
                co_return false;
            }

            auto second = co_await db.acquire();
            expect(!second.has_value()) << "a second lease was handed out from a pool of one";
            if (!second) {
                expect(second.error().code == errc::query_canceled);
            }
            co_return true;
        }());

        expect(ran);
    };

    "timed-out acquire is immediately removed from the waiter queue"_test = [&url] {
        asio::io_context ctx;
        atlas::pool db{ctx.get_executor(), config_for(*url, 1, 10ms)};

        const bool ran = run_on(ctx, [&]() -> asio::awaitable<bool> {
            auto held = co_await db.acquire();
            expect(held.has_value() >> fatal);
            if (!held) {
                co_return false;
            }

            auto timed_out = co_await db.acquire();
            expect(!timed_out.has_value());
            if (!timed_out) {
                expect(timed_out.error().code == errc::query_canceled);
            }

            const auto waiter_count = co_await atlas::detail::pool_test_access::waiter_count(db);
            expect(waiter_count == 0_ul) << "the timed-out acquire remained queued";
            co_return waiter_count == 0;
        }());

        expect(ran);
    };

    "pending intrinsic cancellation removes its queued waiter"_test = [&url] {
        asio::io_context pool_ctx;
        atlas::pool db{pool_ctx.get_executor(), config_for(*url, 1, 100ms)};
        std::optional<atlas::pool_connection> held;

        const bool acquired_initially = run_on(pool_ctx, [&]() -> asio::awaitable<bool> {
            auto acquired = co_await db.acquire();
            expect(acquired.has_value());
            if (!acquired) {
                co_return false;
            }
            held.emplace(std::move(*acquired));
            co_return true;
        }());
        expect(acquired_initially >> fatal);
        if (!acquired_initially) {
            return;
        }

        const auto outcome = cancel_on_waiter_enqueue(pool_ctx, db);
        expect(outcome.completed);
        expect(outcome.failure != nullptr);
        expect(outcome.operation_aborted);

        pool_ctx.restart();
        const auto queued = run_on(pool_ctx, [&]() -> asio::awaitable<std::size_t> {
            co_return co_await atlas::detail::pool_test_access::waiter_count(db);
        }());
        expect(queued == 0_ul) << "intrinsic cancellation orphaned a queued acquire";

        held.reset();
        pool_ctx.restart();
        const bool usable = run_on(pool_ctx, [&]() -> asio::awaitable<bool> {
            auto acquired = co_await db.acquire();
            expect(acquired.has_value()) << (acquired ? "" : acquired.error().message);
            if (!acquired) {
                co_return false;
            }
            auto result = co_await acquired->execute("SELECT 1", no_params);
            expect(result.has_value()) << (result ? "" : result.error().message);
            co_return result.has_value();
        }());
        expect(usable);
    };

    "timer and handoff boundary never loses the pool slot"_test = [&url] {
        asio::io_context ctx;
        atlas::pool db{ctx.get_executor(), config_for(*url, 1, 10ms)};

        const bool ran = run_on(ctx, [&]() -> asio::awaitable<bool> {
            for (int iteration = 0; iteration < 100; ++iteration) {
                auto acquired = co_await db.acquire();
                expect(acquired.has_value() >> fatal);
                if (!acquired) {
                    co_return false;
                }
                std::optional<atlas::pool_connection> held{std::move(*acquired)};

                auto release = [&held]() -> asio::awaitable<void> {
                    co_await sleep_for(10ms);
                    held.reset();
                };
                asio::co_spawn(co_await asio::this_coro::executor, release(), asio::detached);

                {
                    auto boundary = co_await db.acquire();
                    if (!boundary) {
                        expect(boundary.error().code == errc::query_canceled);
                    }
                }

                const bool returned = co_await atlas_test::wait_until([&db] { return db.available() == 1; }, 1s);
                expect(returned) << "the only pool slot was lost at the timer/handoff boundary";
                if (!returned) {
                    co_return false;
                }
            }

            auto usable = co_await db.execute("SELECT 1", no_params);
            expect(usable.has_value()) << (usable ? "" : usable.error().message);
            co_return usable.has_value();
        }());

        expect(ran);
    };
};
