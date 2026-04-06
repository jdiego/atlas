#pragma once

// atlas/schema/table.hpp
//
// Aggregates columns into a named table descriptor.
//
// entity_type is exposed as a public alias so that storage_t can map an
// Entity type to the right table at compile time using if constexpr.
//
// for_each_column drives both DDL generation (ddl.hpp) and serde
// (serde.hpp): it visits every column_t in declaration order, which
// guarantees that SQL column order and parameter array order are identical.
//
// find_column is used by references_t resolution in DDL: given a member
// pointer to the referenced column, it locates the column_t that holds its
// name and type information. Also exposed as a free function for ergonomics.

#include <cstddef>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

#include "atlas/schema/column.hpp"

namespace atlas {

// Forward-declare the recursive helper so table_t can call it.
namespace detail {

template<std::size_t I, typename MemberPtr, typename Entity, typename... Columns>
constexpr const auto& find_column_impl(
    const std::tuple<Columns...>& cols, MemberPtr ptr);

} // namespace detail

// ---------------------------------------------------------------------------
// table_t
// ---------------------------------------------------------------------------

template<typename Entity, typename... Columns>
struct table_t {
    using entity_type = Entity;

    std::string_view       name;
    std::tuple<Columns...> columns;

    static constexpr std::size_t column_count = sizeof...(Columns);

    // Invoke f(column) for each column in declaration order.
    template<typename F>
    constexpr void for_each_column(F&& f) const {
        std::apply([&](const auto&... col) {
            (f(col), ...);
        }, columns);
    }

    // Return a const reference to the column whose member_ptr == ptr.
    // Static-asserts if no column matches (compile-time error).
    template<typename MemberPtr>
    constexpr const auto& find_column(MemberPtr ptr) const {
        return detail::find_column_impl<0>(columns, ptr);
    }
};

// ---------------------------------------------------------------------------
// Detail: recursive column search
// ---------------------------------------------------------------------------

namespace detail {

template<std::size_t I, typename MemberPtr, typename ColTuple>
constexpr const auto& find_column_impl(const ColTuple& cols, MemberPtr ptr) {
    constexpr std::size_t N = std::tuple_size_v<ColTuple>;
    if constexpr (I >= N) {
        static_assert(I < N,
            "find_column: member pointer not found in table");
        // Unreachable — satisfies return type requirement.
        return std::get<0>(cols);
    } else {
        const auto& col = std::get<I>(cols);
        if constexpr (std::is_same_v<decltype(col.member_ptr), MemberPtr>) {
            if (col.member_ptr == ptr) {
                return col;
            }
        }
        return find_column_impl<I + 1>(cols, ptr);
    }
}

} // namespace detail

// Free function overload — delegates to the member for ergonomics.
template<typename Entity, typename... Columns, typename MemberPtr>
constexpr const auto& find_column(
    const table_t<Entity, Columns...>& tbl, MemberPtr ptr)
{
    return detail::find_column_impl<0>(tbl.columns, ptr);
}

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------

template<typename Entity, typename... Columns>
constexpr auto make_table(std::string_view name, Columns&&... cols)
    -> table_t<Entity, std::remove_cvref_t<Columns>...>
{
    static_assert((is_column<Columns> && ...),
        "make_table: all arguments after name must satisfy is_column");
    return {
        name,
        std::tuple<std::remove_cvref_t<Columns>...>(std::forward<Columns>(cols)...)
    };
}

// ---------------------------------------------------------------------------
// Concept
// ---------------------------------------------------------------------------

namespace detail {

template<typename T>
struct is_table_impl : std::false_type {};

template<typename Entity, typename... Columns>
struct is_table_impl<table_t<Entity, Columns...>> : std::true_type {};

} // namespace detail

template<typename T>
concept is_table = detail::is_table_impl<std::remove_cvref_t<T>>::value;

} // namespace atlas
