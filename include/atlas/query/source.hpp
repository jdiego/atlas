#pragma once
//
// Table source identity for query builder nodes.
//
// A plain entity type identifies the default table instance. table_instance
// identifies a specific logical instance of the same mapped table, carrying an
// optional Tag used by serializers to choose the SQL alias.

#include <type_traits>

namespace atlas {

template <typename Entity, typename Tag = void>
struct table_instance {
    using entity_type = Entity;
    using tag_type = Tag;
};

namespace detail {

template <typename T>
struct is_table_instance_impl : std::false_type {};

template <typename Entity, typename Tag>
struct is_table_instance_impl<table_instance<Entity, Tag>> : std::true_type {};

template <typename T>
inline constexpr bool is_table_instance_v =
    is_table_instance_impl<std::remove_cvref_t<T>>::value;

template <typename Source>
struct source_traits {
    using entity_type = std::remove_cvref_t<Source>;
    using tag_type = void;
};

template <typename Entity, typename Tag>
struct source_traits<table_instance<Entity, Tag>> {
    using entity_type = Entity;
    using tag_type = Tag;
};

template <typename Source>
using source_entity_t = typename source_traits<std::remove_cvref_t<Source>>::entity_type;

template <typename Source>
using source_tag_t = typename source_traits<std::remove_cvref_t<Source>>::tag_type;

template <typename Entity, typename Tag>
using source_from_parts_t =
    std::conditional_t<std::is_void_v<Tag>, Entity, table_instance<Entity, Tag>>;

template <typename Source, typename Tag = void>
struct qualify_source {
    static_assert(!is_table_instance_v<Source> || std::is_void_v<Tag>,
        "table_instance already carries a tag; do not pass a second Tag");

    using type = std::conditional_t<
        is_table_instance_v<Source>,
        std::remove_cvref_t<Source>,
        source_from_parts_t<std::remove_cvref_t<Source>, Tag>>;
};

template <typename Source, typename Tag = void>
using qualify_source_t = typename qualify_source<Source, Tag>::type;

template <typename Source>
using canonical_source_t =
    source_from_parts_t<source_entity_t<Source>, source_tag_t<Source>>;

} // namespace detail

template <typename T>
concept is_table_instance = detail::is_table_instance_v<T>;

} // namespace atlas
