// Exercises atlas::transaction against a real server. Transaction semantics are
// only observable through the database, so this whole file needs
// ATLAS_TEST_CONNINFO.

#include "async_test_support.hpp"

#include "atlas/async/pool.hpp"
#include "atlas/async/transaction.hpp"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/ut.hpp>

#include <chrono>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace ut = boost::ut;
namespace asio = boost::asio;
using namespace std::chrono_literals;

namespace {

using atlas::pg::errc;
using atlas_test::conninfo;
using atlas_test::run_on;
using atlas_test::sleep_for;

constexpr std::span<const char *const> no_params{};

[[nodiscard]] auto config_for(std::string url, std::size_t max_size) -> atlas::pool_config {
    atlas::pool_config cfg;
    cfg.url = std::move(url);
    cfg.max_size = max_size;
    cfg.timeout = 5s;
    return cfg;
}

// Reports how many rows the table holds, or -1 if the query failed.
[[nodiscard]] auto count_rows(atlas::pool &db, std::string_view table) -> asio::awaitable<long> {
    std::string sql{"SELECT count(*) FROM "};
    sql.append(table);

    auto res = co_await db.execute(sql, no_params);
    if (!res) {
        co_return -1;
    }

    auto field = res->get(0, 0);
    if (!field || !field->has_value()) {
        co_return -1;
    }
    co_return std::stol(std::string{**field});
}

[[nodiscard]] auto reset_table(atlas::pool &db, std::string_view table) -> asio::awaitable<bool> {
    std::string drop{"DROP TABLE IF EXISTS "};
    drop.append(table);
    auto dropped = co_await db.execute(drop, no_params);
    if (!dropped) {
        co_return false;
    }

    std::string create{"CREATE TABLE "};
    create.append(table).append(" (id int)");
    auto created = co_await db.execute(create, no_params);
    co_return created.has_value();
}

} // namespace

ut::suite<"async/transaction/integration"> transaction_suite = [] {
    using namespace ut;

    const auto url = conninfo();
    if (!url) {
        // Reported rather than silently contributing zero tests, so a CI run
        // without a server is visibly uncovered instead of looking green.
        skip / "requires a live server via ATLAS_TEST_CONNINFO"_test = [] {};
        return;
    }

    "a committed transaction persists its writes"_test = [&url] {
        asio::io_context ctx;
        atlas::pool db{ctx.get_executor(), config_for(*url, 2)};

        const bool ran = run_on(ctx, [&]() -> asio::awaitable<bool> {
            expect(co_await reset_table(db, "atlas_tx_commit"));

            auto tx = co_await db.begin();
            expect(tx.has_value()) << (tx ? "" : tx.error().message);
            if (!tx) {
                co_return false;
            }

            auto inserted = co_await tx->execute("INSERT INTO atlas_tx_commit VALUES (1)", no_params);
            expect(inserted.has_value());

            auto committed = co_await tx->commit();
            expect(committed.has_value());

            expect(co_await count_rows(db, "atlas_tx_commit") == 1L);
            co_return true;
        }());

        expect(ran);
    };

    "an explicit rollback discards its writes"_test = [&url] {
        asio::io_context ctx;
        atlas::pool db{ctx.get_executor(), config_for(*url, 2)};

        const bool ran = run_on(ctx, [&]() -> asio::awaitable<bool> {
            expect(co_await reset_table(db, "atlas_tx_rollback"));

            auto tx = co_await db.begin();
            if (!tx) {
                expect(false) << tx.error().message;
                co_return false;
            }

            auto inserted = co_await tx->execute("INSERT INTO atlas_tx_rollback VALUES (1)", no_params);
            expect(inserted.has_value());

            auto rolled_back = co_await tx->rollback();
            expect(rolled_back.has_value());

            expect(co_await count_rows(db, "atlas_tx_rollback") == 0L);
            co_return true;
        }());

        expect(ran);
    };

    "an explicit rollback reports a terminated backend"_test = [&url] {
        asio::io_context ctx;
        atlas::pool db{ctx.get_executor(), config_for(*url, 2)};

        const bool ran = run_on(ctx, [&]() -> asio::awaitable<bool> {
            auto tx = co_await db.begin();
            if (!tx) {
                expect(false) << tx.error().message;
                co_return false;
            }

            auto backend = co_await tx->execute("SELECT pg_backend_pid()", no_params);
            expect(backend.has_value()) << (backend ? "" : backend.error().message);
            if (!backend) {
                co_return false;
            }

            auto field = backend->get(0, 0);
            expect(field.has_value());
            expect(field && field->has_value());
            if (!field || !field->has_value()) {
                co_return false;
            }

            std::string terminate{"SELECT pg_terminate_backend("};
            terminate.append(**field).append(")");
            auto terminated = co_await db.execute(terminate, no_params);
            expect(terminated.has_value()) << (terminated ? "" : terminated.error().message);
            co_await sleep_for(50ms);

            auto rolled_back = co_await tx->rollback();
            expect(!rolled_back.has_value()) << "explicit rollback suppressed the transport failure";
            if (!rolled_back) {
                expect(!rolled_back.error().message.empty()) << "the rollback error lost its server message";
            }
            co_return true;
        }());

        expect(ran);
    };

    "dropping an uncommitted transaction rolls it back"_test = [&url] {
        asio::io_context ctx;
        atlas::pool db{ctx.get_executor(), config_for(*url, 2)};

        const bool ran = run_on(ctx, [&]() -> asio::awaitable<bool> {
            expect(co_await reset_table(db, "atlas_tx_dtor"));

            {
                auto tx = co_await db.begin();
                if (!tx) {
                    expect(false) << tx.error().message;
                    co_return false;
                }

                auto inserted = co_await tx->execute("INSERT INTO atlas_tx_dtor VALUES (1)", no_params);
                expect(inserted.has_value());
            } // the destructor fires ROLLBACK detached

            co_await sleep_for(200ms);
            expect(co_await count_rows(db, "atlas_tx_dtor") == 0L);
            co_return true;
        }());

        expect(ran);
    };

    "commit on an already finished transaction is rejected"_test = [&url] {
        asio::io_context ctx;
        atlas::pool db{ctx.get_executor(), config_for(*url, 2)};

        const bool ran = run_on(ctx, [&]() -> asio::awaitable<bool> {
            auto tx = co_await db.begin();
            if (!tx) {
                expect(false) << tx.error().message;
                co_return false;
            }

            expect((co_await tx->commit()).has_value());

            auto again = co_await tx->commit();
            expect(!again.has_value());
            if (!again) {
                expect(again.error().code == errc::invalid_state);
            }
            co_return true;
        }());

        expect(ran);
    };

    "rollback_to undoes only the work after the savepoint"_test = [&url] {
        asio::io_context ctx;
        atlas::pool db{ctx.get_executor(), config_for(*url, 2)};

        const bool ran = run_on(ctx, [&]() -> asio::awaitable<bool> {
            expect(co_await reset_table(db, "atlas_tx_savepoint"));

            auto tx = co_await db.begin();
            if (!tx) {
                expect(false) << tx.error().message;
                co_return false;
            }

            expect((co_await tx->execute("INSERT INTO atlas_tx_savepoint VALUES (1)", no_params)).has_value());

            auto marked = co_await tx->savepoint("checkpoint");
            expect(marked.has_value()) << (marked ? "" : marked.error().message);

            expect((co_await tx->execute("INSERT INTO atlas_tx_savepoint VALUES (2)", no_params)).has_value());

            auto reverted = co_await tx->rollback_to("checkpoint");
            expect(reverted.has_value()) << (reverted ? "" : reverted.error().message);

            auto released = co_await tx->release_savepoint("checkpoint");
            expect(released.has_value()) << (released ? "" : released.error().message);

            expect((co_await tx->commit()).has_value());

            expect(co_await count_rows(db, "atlas_tx_savepoint") == 1L);
            co_return true;
        }());

        expect(ran);
    };

    // Regression: savepoint names were concatenated straight into the statement,
    // so a name carrying a quote and a semicolon ran as SQL.
    "a savepoint name carrying SQL is treated as an identifier"_test = [&url] {
        asio::io_context ctx;
        atlas::pool db{ctx.get_executor(), config_for(*url, 2)};

        const bool ran = run_on(ctx, [&]() -> asio::awaitable<bool> {
            expect(co_await reset_table(db, "atlas_tx_injection"));

            auto tx = co_await db.begin();
            if (!tx) {
                expect(false) << tx.error().message;
                co_return false;
            }

            const std::string payload = R"(sp"; DROP TABLE atlas_tx_injection; --)";

            auto marked = co_await tx->savepoint(payload);
            expect(marked.has_value()) << (marked ? "" : marked.error().message);

            auto reverted = co_await tx->rollback_to(payload);
            expect(reverted.has_value()) << (reverted ? "" : reverted.error().message);

            expect((co_await tx->commit()).has_value());

            // The table survives only if the payload never left identifier position.
            expect(co_await count_rows(db, "atlas_tx_injection") == 0L) << "the injected statement ran";
            co_return true;
        }());

        expect(ran);
    };

    "an unusable savepoint name is rejected before it reaches the server"_test = [&url] {
        asio::io_context ctx;
        atlas::pool db{ctx.get_executor(), config_for(*url, 2)};

        const bool ran = run_on(ctx, [&]() -> asio::awaitable<bool> {
            auto tx = co_await db.begin();
            if (!tx) {
                expect(false) << tx.error().message;
                co_return false;
            }

            auto empty = co_await tx->savepoint("");
            expect(!empty.has_value());
            if (!empty) {
                expect(empty.error().code == errc::invalid_argument);
            }

            using namespace std::string_view_literals;
            auto embedded_null = co_await tx->savepoint("a\0b"sv);
            expect(!embedded_null.has_value());
            if (!embedded_null) {
                expect(embedded_null.error().code == errc::invalid_argument);
            }

            expect((co_await tx->rollback()).has_value());
            co_return true;
        }());

        expect(ran);
    };
};
