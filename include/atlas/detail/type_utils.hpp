#pragma once

#include <tuple>

namespace atlas::detail {

// Appends a single type to a std::tuple type.
template <typename Tuple, typename Elem>
struct tuple_append;

template <typename... Ts, typename Elem>
struct tuple_append<std::tuple<Ts...>, Elem> {
    using type = std::tuple<Ts..., Elem>;
};

template <typename Tuple, typename Elem>
using tuple_append_t = typename tuple_append<Tuple, Elem>::type;

} // namespace atlas::detail
