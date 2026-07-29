#pragma once

#include <libpq-fe.h>

namespace atlas::detail {

enum class connect_poll_action {
    wait_read,
    wait_write,
    poll_again,
    connected,
    failed,
};

inline constexpr auto initial_connect_poll_status = PGRES_POLLING_WRITING;

[[nodiscard]] constexpr auto connect_poll_action_for(PostgresPollingStatusType status) noexcept -> connect_poll_action {
    switch (status) {
    case PGRES_POLLING_READING:
        return connect_poll_action::wait_read;
    case PGRES_POLLING_WRITING:
        return connect_poll_action::wait_write;
    case PGRES_POLLING_OK:
        return connect_poll_action::connected;
    case PGRES_POLLING_FAILED:
        return connect_poll_action::failed;
    default:
        return connect_poll_action::poll_again;
    }
}

} // namespace atlas::detail
