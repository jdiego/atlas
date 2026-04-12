#pragma once
//
// Represents a single mapped column as a compile-time object.
//
// member_type is deduced from the member pointer via the helper trait
// member_ptr_traits<MemberPtr>. Given T Entity::*, member_type is T.
//
// has_constraint<C>() uses a fold expression over the Constraints pack:
//   (std::is_same_v<C, Constraints> || ...) — O(1) compile time.
//
// get() and set() are the bridge between the DSL layer and runtime serde:
// they read/write an entity field using the stored member pointer, so serde
// code never needs to know about the concrete member address directly.

#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

#include "atlas/schema/constraints.hpp"

namespace atlas {

// ---------------------------------------------------------------------------
// Internal trait: extract member type from a member pointer type.
// E.g. member_ptr_traits<int32_t User::*>::type == int32_t
// ---------------------------------------------------------------------------
namespace detail {

template<typename MemberPtr>
struct member_ptr_traits;

template<typename T, typename Entity>
struct member_ptr_traits<T Entity::*> {
    using type        = T;
    using entity_type = Entity;
};

} // namespace detail

// ---------------------------------------------------------------------------
// column_t
// ---------------------------------------------------------------------------

template<typename Entity,
         typename MemberPtr,
         typename... Constraints>
struct column_t {
    using member_type = typename detail::member_ptr_traits<MemberPtr>::type;

    std::string_view           name;
    MemberPtr                  member_ptr;
    std::tuple<Constraints...> constraints;

    // True iff Constraints... contains a type equal to C.
    template<typename C>
    static constexpr bool has_constraint() noexcept {
        return (std::is_same_v<C, Constraints> || ...);
    }

    // Returns a const ref to the first constraint of type C.
    // Only well-formed when has_constraint<C>() is true.
    template<typename C>
    constexpr const C& get_constraint() const noexcept {
        return std::get<C>(constraints);
    }

    // Read the mapped member from an entity instance.
    constexpr const member_type& get(const Entity& e) const noexcept {
        return e.*member_ptr;
    }

    // Write the mapped member to an entity instance.
    constexpr void set(Entity& e, member_type&& v) const noexcept {
        e.*member_ptr = std::move(v);
    }
};

// ---------------------------------------------------------------------------
// Factory function + deduction guide
// ---------------------------------------------------------------------------

template<typename Entity, typename T, typename... Constraints>
constexpr auto make_column(
    std::string_view    name,
    T Entity::*         member_ptr,
    Constraints&&...    cs)
    -> column_t<Entity, T Entity::*, std::remove_cvref_t<Constraints>...>
{
    return {
        name,
        member_ptr,
        std::tuple<std::remove_cvref_t<Constraints>...>(std::forward<Constraints>(cs)...)
    };
}

// ---------------------------------------------------------------------------
// Concept: detects a column_t specialization.
// ---------------------------------------------------------------------------

namespace detail {

template<typename C>
struct is_column_impl : std::false_type {};

template<typename Entity, typename MemberPtr, typename... Constraints>
struct is_column_impl<column_t<Entity, MemberPtr, Constraints...>> : std::true_type {};

} // namespace detail

template<typename C>
concept is_column = detail::is_column_impl<std::remove_cvref_t<C>>::value;

} // namespace atlas
