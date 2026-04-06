#pragma once

// atlas/schema/ddl.hpp
//
// Generates CREATE TABLE SQL from a table_t descriptor.
//
// Column clause ordering within each column definition:
//   <name> <SQL_TYPE> NOT NULL [PRIMARY KEY] [UNIQUE] [DEFAULT <v>] [REFERENCES ...]
//
// NOT NULL appears before PRIMARY KEY because PostgreSQL accepts both orderings
// but the canonical style puts nullability first. PRIMARY KEY implies NOT NULL
// in PostgreSQL, but we emit it explicitly for clarity and portability.
//
// Two overloads for REFERENCES resolution:
//   create_table_sql(table)          — no storage available; emits a TODO
//                                      comment in place of REFERENCES clause
//   create_table_sql(table, storage) — storage is used to resolve RefEntity
//                                      to a table name and find the referenced
//                                      column name via find_column().
//
// Future extension point: ALTER TABLE / DROP TABLE generation for a migration
// DSL would follow the same table_t / storage_t inputs; add free functions
// alter_table_sql() and drop_table_sql() in this header or a new ddl_migrate.hpp.

#include <format>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>

#include "atlas/schema/column.hpp"
#include "atlas/schema/constraints.hpp"
#include "atlas/schema/storage.hpp"
#include "atlas/schema/table.hpp"
#include "atlas/schema/type_traits.hpp"

namespace atlas {

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

namespace detail {

// Format a default value for SQL output.
// Numeric types use std::to_string; std::string gets single-quoted.
template<typename T>
std::string format_default(const T& v) {
    if constexpr (std::is_same_v<T, std::string>) {
        return std::string("'") + v + "'";
    } else if constexpr (std::is_same_v<T, bool>) {
        return v ? "TRUE" : "FALSE";
    } else {
        return std::to_string(v);
    }
}

// Build a single column definition line.
// StoragePtr is either nullptr_t (no storage) or a pointer to storage_t.
template<typename ColT, typename StoragePtr>
std::string column_def(const ColT& col, StoragePtr storage_ptr) {
    using member_type = typename ColT::member_type;

    std::string line;
    line += "  ";
    line += col.name;
    line += ' ';
    line += std::string(pg_type<member_type>::sql_name);
    line += " NOT NULL";

    if constexpr (ColT::template has_constraint<primary_key_t>()) {
        line += " PRIMARY KEY";
    }
    if constexpr (ColT::template has_constraint<unique_t>()) {
        line += " UNIQUE";
    }
    if constexpr (ColT::template has_constraint<default_value_t<member_type>>()) {
        const auto& dv = col.template get_constraint<default_value_t<member_type>>();
        line += " DEFAULT ";
        line += format_default(dv.value);
    }

    // REFERENCES clause — resolved only when storage is provided.
    // We detect references_t by scanning the constraints tuple.
    auto emit_references = [&]<typename C>(const C& constraint) {
        if constexpr (is_references<C>) {
            if constexpr (std::is_null_pointer_v<StoragePtr>) {
                // No storage — emit a placeholder so the SQL is still parseable
                // as a comment; developers should pass storage for full DDL.
                line += " /* TODO: REFERENCES unresolved — pass storage to create_table_sql */";
            } else {
                // Resolve the referenced table and column names at compile time.
                using ref_entity = typename std::remove_cvref_t<C>::ref_entity_type;
                const auto& ref_table  = storage_ptr->template get_table<ref_entity>();
                const auto& ref_col    = find_column(ref_table, constraint.column_ptr);
                line += " REFERENCES ";
                line += ref_table.name;
                line += '(';
                line += ref_col.name;
                line += ')';
            }
        }
    };

    std::apply([&](const auto&... c) {
        (emit_references(c), ...);
    }, col.constraints);

    return line;
}

} // namespace detail

// ---------------------------------------------------------------------------
// Overload 1: no storage — REFERENCES emits TODO comment
// ---------------------------------------------------------------------------

template<typename Entity, typename... Columns>
[[nodiscard]] std::string
create_table_sql(const table_t<Entity, Columns...>& table) {
    std::string sql;
    sql += "CREATE TABLE IF NOT EXISTS ";
    sql += table.name;
    sql += " (\n";

    std::size_t idx = 0;
    table.for_each_column([&](const auto& col) {
        sql += detail::column_def(col, nullptr);
        if (++idx < table.column_count) sql += ',';
        sql += '\n';
    });

    sql += ");";
    return sql;
}

// ---------------------------------------------------------------------------
// Overload 2: with storage — REFERENCES fully resolved
// ---------------------------------------------------------------------------

template<typename Entity, typename... Columns, typename... Tables>
[[nodiscard]] std::string
create_table_sql(const table_t<Entity, Columns...>& table,
                 const storage_t<Tables...>& storage)
{
    std::string sql;
    sql += "CREATE TABLE IF NOT EXISTS ";
    sql += table.name;
    sql += " (\n";

    std::size_t idx = 0;
    table.for_each_column([&](const auto& col) {
        sql += detail::column_def(col, &storage);
        if (++idx < table.column_count) sql += ',';
        sql += '\n';
    });

    sql += ");";
    return sql;
}

} // namespace atlas
