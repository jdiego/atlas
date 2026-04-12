#pragma once

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
#include <functional>
#include <optional>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

#include "atlas/schema/column.hpp"

namespace atlas {

namespace detail {
template<typename... Columns>
struct column_ref {
    using variant_type = std::variant<std::reference_wrapper<const Columns>...>;

    explicit constexpr column_ref(variant_type column) noexcept
        : name(std::visit([](const auto& ref) { return ref.get().name; }, column)),
          column(column) {}

    template<typename Constraint>
    [[nodiscard]] constexpr auto has_constraint() const noexcept -> bool {
        return std::visit([](const auto& ref) {
            return ref.get().template has_constraint<Constraint>();
        }, column);
    }

    std::string_view name;

private:
    variant_type column;
};

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

    // Invoke fn(conlumn) for each column in declaration order.
    template<typename F>
    constexpr void for_each_column(F&& fn) const {
        std::apply([&](const auto&... col) {
            (fn(col), ...);
        }, columns);
    }

    // Return a typed proxy to the column whose member_ptr == ptr.
    template<typename MemberPtr>
    [[nodiscard]] constexpr auto find_column(MemberPtr ptr) const {
        using result_type = detail::column_ref<Columns...>;
        std::optional<typename result_type::variant_type> found;

        std::apply([&](const auto&... col) {
            ([&] {
                if constexpr (std::is_same_v<decltype(col.member_ptr), MemberPtr>) {
                    if (!found && col.member_ptr == ptr) {
                        found.emplace(std::cref(col));
                    }
                }
            }(), ...);
        }, columns);

        if (!found) {
            throw "find_column: member pointer not found in this table";
        }

        return result_type{*found};
    }
};

// ---------------------------------------------------------------------------
// Free function overload — delegates to member for ergonomics.
// ---------------------------------------------------------------------------
 
template<typename Entity, typename... Columns, typename MemberPtr>
[[nodiscard]] constexpr auto find_column(const table_t<Entity, Columns...>& tbl, MemberPtr ptr)
{
    return tbl.find_column(ptr);
}

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------

template<typename Entity, typename... Columns>
constexpr auto make_table(std::string_view name, Columns&&... cols) -> table_t<Entity, std::remove_cvref_t<Columns>...>
{
    static_assert((is_column<Columns> && ...), "all arguments after name must satisfy is_column");
    return { name, std::tuple<std::remove_cvref_t<Columns>...>(std::forward<Columns>(cols)...)};
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
