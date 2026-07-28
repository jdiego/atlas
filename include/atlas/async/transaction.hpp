#pragma once

#include "atlas/async/pool.hpp"
#include "atlas/pg/error.hpp"
#include "atlas/pg/result.hpp"

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>

#include <expected>
#include <span>
#include <string_view>

namespace atlas {

namespace asio = boost::asio;

// executor_type forward-declared; defined in async_connection.hpp.
using executor_type = asio::any_io_executor;

class transaction {
public:
    transaction(const transaction &) = delete;
    transaction &operator=(const transaction &) = delete;
    transaction(transaction &&) noexcept;
    transaction &operator=(transaction &&) noexcept;

    // If !committed_: fire-and-forget co_spawn(do_rollback(), detached).
    // Prevents leaving dangling server-side transactions on scope exit.
    ~transaction();

    // Sends COMMIT. Sets committed_ = true on success.
    [[nodiscard]] asio::awaitable<std::expected<void, pg::error>> commit();

    // Sends ROLLBACK and propagates errors. Marks the transaction finished only
    // after the server acknowledges it.
    [[nodiscard]] asio::awaitable<std::expected<void, pg::error>> rollback();

    // Executes a query within this open transaction.
    [[nodiscard]] asio::awaitable<std::expected<pg::result, pg::error>> execute(std::string_view sql,
                                                                                std::span<const char *const> params);

    // Sends SAVEPOINT <name>.
    [[nodiscard]] asio::awaitable<std::expected<void, pg::error>> savepoint(std::string_view name);

    // Sends ROLLBACK TO SAVEPOINT <name>.
    [[nodiscard]] asio::awaitable<std::expected<void, pg::error>> rollback_to(std::string_view name);

    // Sends RELEASE SAVEPOINT <name>.
    [[nodiscard]] asio::awaitable<std::expected<void, pg::error>> release_savepoint(std::string_view name);

private:
    friend class pool;
    explicit transaction(pool_connection conn, executor_type ex);

    pool_connection conn_;
    executor_type ex_;
    bool committed_ = false;

    // Best-effort ROLLBACK used only by destructor cleanup.
    asio::awaitable<void> do_rollback();
};

} // namespace atlas
