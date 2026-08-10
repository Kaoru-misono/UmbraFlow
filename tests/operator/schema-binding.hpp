#pragma once

// Binds a journal record's stored shape to the schema that declares it.
//
// schema/umbraflow-journal-v1.schema.json is not readable at runtime, and the
// Operator stores JR:`JournalEvent` and JR:`ProjectState` as SQLite rows rather
// than as JSON documents, so nothing in the framework can compare the two. A
// gate that reads the schema text alone passes whether or not the DDL agrees,
// which is how `canonical_event` came to name bytes that are not an event. The
// helpers below let a case ask the database the Operator actually created what
// its columns are, and compare that with the schema's own `required` list.

#include <operator/ledger.hpp>

#include <doctest/doctest.h>

#include <sqlite3.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace uf::operator_runtime::test_support
{
    struct SqliteClose final
    {
        auto operator()(sqlite3* p_database) const noexcept -> void
        {
            static_cast<void>(sqlite3_close(p_database));
        }
    };

    struct SqliteFinalize final
    {
        auto operator()(sqlite3_stmt* p_statement) const noexcept -> void
        {
            static_cast<void>(sqlite3_finalize(p_statement));
        }
    };

    // The column names SQLite reports for one table of an Operator database,
    // sorted. The coordinator holds the file under PRAGMA locking_mode=
    // EXCLUSIVE for its whole lifetime, so this opens a runtime directory of
    // its own, closes it, and reads the file the Operator's DDL left behind.
    // Nothing here can be satisfied by source text.
    [[nodiscard]]
    inline auto operatorTableColumns(
        std::filesystem::path const& runtimeDirectory,
        std::string_view table
    ) -> std::vector<std::string>
    {
        auto const databasePath = [&runtimeDirectory]
        {
            auto store = OperatorCoordinator::open(runtimeDirectory);
            REQUIRE(store.has_value());
            return store->databasePath();
        }();

        auto* p_openedDatabase = static_cast<sqlite3*>(nullptr);
        auto const opened = sqlite3_open_v2(
            databasePath.string().c_str(),
            &p_openedDatabase,
            SQLITE_OPEN_READONLY,
            nullptr
        );
        auto database = std::unique_ptr<sqlite3, SqliteClose>{p_openedDatabase};
        REQUIRE(opened == SQLITE_OK);
        REQUIRE(database != nullptr);

        auto* p_preparedStatement = static_cast<sqlite3_stmt*>(nullptr);
        auto const prepared = sqlite3_prepare_v2(
            database.get(),
            "SELECT name FROM pragma_table_info(?1)",
            -1,
            &p_preparedStatement,
            nullptr
        );
        auto statement = std::unique_ptr<sqlite3_stmt, SqliteFinalize>{
            p_preparedStatement,
        };
        REQUIRE(prepared == SQLITE_OK);
        REQUIRE(statement != nullptr);
        REQUIRE(
            sqlite3_bind_text(
                statement.get(),
                1,
                table.data(),
                static_cast<int>(table.size()),
                SQLITE_TRANSIENT
            ) == SQLITE_OK
        );

        auto columns = std::vector<std::string>{};
        auto step    = sqlite3_step(statement.get());
        while (step == SQLITE_ROW)
        {
            auto const* p_name = sqlite3_column_text(statement.get(), 0);
            REQUIRE(p_name != nullptr);
            columns.emplace_back(
                std::string{
                    p_name,
                    p_name + sqlite3_column_bytes(statement.get(), 0),
                }
            );
            step = sqlite3_step(statement.get());
        }
        REQUIRE(step == SQLITE_DONE);
        std::ranges::sort(columns);
        return columns;
    }

    // The member names one `$defs` entry lists under `required`, sorted. The
    // argument is the definition object's exact text, as the case's own
    // definition() reader returns it.
    [[nodiscard]]
    inline auto requiredMembers(
        std::string_view definitionText
    ) -> std::vector<std::string>
    {
        auto const declaration = std::string_view{"\"required\": ["};
        auto const declaredAt  = definitionText.find(declaration);
        REQUIRE(declaredAt != std::string_view::npos);

        auto members = std::vector<std::string>{};
        auto rest    = definitionText.substr(declaredAt + declaration.size());
        auto const closedAt = rest.find(']');
        REQUIRE(closedAt != std::string_view::npos);
        rest = rest.substr(0U, closedAt);
        while (true)
        {
            auto const opened = rest.find('"');
            if (opened == std::string_view::npos)
            {
                break;
            }
            rest = rest.substr(opened + 1U);
            auto const closed = rest.find('"');
            REQUIRE(closed != std::string_view::npos);
            members.emplace_back(rest.substr(0U, closed));
            rest = rest.substr(closed + 1U);
        }
        REQUIRE_FALSE(members.empty());
        std::ranges::sort(members);
        return members;
    }
}
