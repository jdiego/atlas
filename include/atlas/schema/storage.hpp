#pragma once

// atlas/schema/storage.hpp
//
// Top-level schema registry that holds all table descriptors.
//
// storage_t owns no heap memory — it is a compile-time index stored entirely
// in a std::tuple of table_t values (which are themselves all constexpr-
// constructible). The whole registry can live in static storage or on the
// stack at zero allocation cost.
//
// get_table<Entity>() uses a recursive if constexpr walk over the Tables
// pack to find the table whose entity_type matches Entity. A static_assert
// fires at compile time if Entity was never registered, preventing silent
// misuse.
//
// Future extension point: a runtime validate() method could compare the
// compile-time schema against the live PostgreSQL catalog (pg_attribute,
// pg_class) and return an error list — without changing this header.

#include <cstddef>
#include <tuple>
#include <type_traits>
#include <utility>

#include "atlas/schema/table.hpp"

namespace atlas {

// ---------------------------------------------------------------------------
// storage_t
// ---------------------------------------------------------------------------

template<typename... Tables>
struct storage_t {
    std::tuple<Tables...> tables;

    static constexpr std::size_t table_count = sizeof...(Tables);

    // Returns a const reference to the table_t whose entity_type == Entity.
    // Static assertion at compile time if Entity is not registered.
    template<typename Entity>
    constexpr const auto& get_table() const noexcept {
        return detail_get_table<Entity, 0>();
    }

    // Invoke f(table) for each registered table in declaration order.
    template<typename F>
    constexpr void for_each_table(F&& f) const {
        std::apply([&](const auto&... tbl) {
            (f(tbl), ...);
        }, tables);
    }

private:
    template<typename Entity, std::size_t I>
    constexpr const auto& detail_get_table() const noexcept {
        static_assert(I < sizeof...(Tables),
            "get_table<Entity>: Entity is not registered in this storage");
        if constexpr (I < sizeof...(Tables)) {
            using TableI = std::tuple_element_t<I, std::tuple<Tables...>>;
            if constexpr (std::is_same_v<typename TableI::entity_type, Entity>) {
                return std::get<I>(tables);
            } else {
                return detail_get_table<Entity, I + 1>();
            }
        }
    }
};

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------

template<typename... Tables>
constexpr auto make_storage(Tables&&... ts) -> storage_t<std::remove_cvref_t<Tables>...> {
    static_assert((is_table<Tables> && ...),
        "make_storage: all arguments must satisfy is_table");
    return { std::tuple<std::remove_cvref_t<Tables>...>(std::forward<Tables>(ts)...) };
}

// ---------------------------------------------------------------------------
// Concept
// ---------------------------------------------------------------------------

namespace detail {

template<typename S>
struct is_storage_impl : std::false_type {};

template<typename... Tables>
struct is_storage_impl<storage_t<Tables...>> : std::true_type {};

} // namespace detail

template<typename S>
concept is_storage = detail::is_storage_impl<std::remove_cvref_t<S>>::value;

} // namespace atlas
