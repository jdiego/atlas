#pragma once

#include <libpq-fe.h>

#include <memory>

namespace atlas::pg::detail {

struct connection_deleter {
    void operator()(PGconn *handle) const noexcept {
        if (handle != nullptr) {
            PQfinish(handle);
        }
    }
};

struct result_deleter {
    void operator()(PGresult *handle) const noexcept {
        if (handle != nullptr) {
            PQclear(handle);
        }
    }
};

using connection_handle = std::unique_ptr<PGconn, connection_deleter>;
using result_handle = std::unique_ptr<PGresult, result_deleter>;

} // namespace atlas::pg::detail
