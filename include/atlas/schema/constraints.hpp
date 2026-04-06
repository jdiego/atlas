#pragma once

// atlas/schema/constraints.hpp
//
// Tag types representing SQL column constraints.
//
// Constraints are pure empty tag types — they carry zero runtime cost.
// The compiler eliminates them entirely; only the type information survives
// in the column_t<...Constraints> parameter pack.
//
// references_t stores a member pointer to the referenced column but defers
// table name resolution to DDL generation time. The storage<> registry (or
// a passed storage reference) is the only place that can map Entity → table
// name at DDL time, so we intentionally do not store the name here.
//
// Multiple constraints compose naturally via the variadic Constraints... pack
// in column_t — there is no special "constraint list" wrapper needed.

#include <concepts>
#include <type_traits>
#include <utility>

namespace atlas {

// ---------------------------------------------------------------------------
// Tag types
// ---------------------------------------------------------------------------

struct primary_key_t {};
struct not_null_t    {};
struct unique_t      {};

// Carries a default value of type T.
template<typename T>
struct default_value_t {
    T value;
};

// Carries a member pointer to the referenced column in RefEntity.
// The table name for RefEntity is resolved at DDL generation time
// via a storage<> instance — this struct only stores the pointer.
template<typename RefEntity, typename RefMemberPtr>
struct references_t {
    using ref_entity_type = RefEntity;
    RefMemberPtr column_ptr;
};

// ---------------------------------------------------------------------------
// Factory functions
// ---------------------------------------------------------------------------

constexpr auto primary_key() noexcept -> primary_key_t { return {}; }
constexpr auto not_null()    noexcept -> not_null_t    { return {}; }
constexpr auto unique()      noexcept -> unique_t      { return {}; }

template<typename T>
constexpr auto default_value(T&& v) -> default_value_t<std::remove_cvref_t<T>> {
    return { std::forward<T>(v) };
}

// Usage: atlas::references<RefEntity>(&RefEntity::member)
template<typename RefEntity, typename RefMemberPtr>
constexpr auto references(RefMemberPtr ptr) -> references_t<RefEntity, RefMemberPtr> {
    return { ptr };
}

// ---------------------------------------------------------------------------
// Detail traits — used to implement the concepts below.
// ---------------------------------------------------------------------------

namespace detail {

template<typename C>
struct is_default_value_impl : std::false_type {};
template<typename T>
struct is_default_value_impl<default_value_t<T>> : std::true_type {};

template<typename C>
struct is_references_impl : std::false_type {};
template<typename RE, typename RMP>
struct is_references_impl<references_t<RE, RMP>> : std::true_type {};

} // namespace detail

// ---------------------------------------------------------------------------
// Concepts
// ---------------------------------------------------------------------------

template<typename C>
concept is_primary_key = std::same_as<std::remove_cvref_t<C>, primary_key_t>;

template<typename C>
concept is_not_null = std::same_as<std::remove_cvref_t<C>, not_null_t>;

template<typename C>
concept is_unique = std::same_as<std::remove_cvref_t<C>, unique_t>;

template<typename C>
concept is_default_value = detail::is_default_value_impl<std::remove_cvref_t<C>>::value;

template<typename C>
concept is_references = detail::is_references_impl<std::remove_cvref_t<C>>::value;

template<typename C>
concept is_constraint =
    is_primary_key<C>  ||
    is_not_null<C>     ||
    is_unique<C>       ||
    is_default_value<C>||
    is_references<C>;

} // namespace atlas
