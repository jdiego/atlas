#pragma once
//
// Convenience single-include header for the Atlas query builder layer.
//
// Include this header to gain access to the full query DSL:
//   atlas::select, atlas::insert, atlas::update, atlas::remove,
//   atlas::eq, atlas::gt, atlas::and_, atlas::or_, atlas::not_,
//   atlas::count, atlas::sum, atlas::avg, atlas::min, atlas::max,
//   atlas::join_kind, serialize_context, serialize_predicate, etc.
//
// No new declarations are introduced here.

#include "atlas/query/aggregate.hpp"
#include "atlas/query/expr.hpp"
#include "atlas/query/insert.hpp"
#include "atlas/query/join.hpp"
#include "atlas/query/predicate.hpp"
#include "atlas/query/remove.hpp"
#include "atlas/query/select.hpp"
#include "atlas/query/sql_serialize.hpp"
#include "atlas/query/update.hpp"
