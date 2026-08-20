#pragma once

#include <core/safety/annotations.hpp>

#include <doctest/doctest.h>

#include "../../../modules/operator/external/sqlite/sqlite3.h"

#include <cstddef>
#include <filesystem>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uf::operator_runtime::test_support
{
    class OperatorDatabaseProbe final
    {
        struct SqliteClose final
        {
            auto operator()(sqlite3* p_database) const noexcept -> void
            {
                // SAFETY: The unique owner closes once; cleanup cannot recover.
                static_cast<void>(sqlite3_close(p_database));
            }
        };

        struct SqliteFinalize final
        {
            auto operator()(sqlite3_stmt* p_statement) const noexcept -> void
            {
                // SAFETY: The unique owner finalizes once; cleanup cannot recover.
                static_cast<void>(sqlite3_finalize(p_statement));
            }
        };

        std::unique_ptr<sqlite3, SqliteClose> m_database{};

    public:
        explicit OperatorDatabaseProbe(
            std::filesystem::path const& databasePath
        )
        {
            sqlite3* p_openedDatabase{};
            auto const opened = sqlite3_open_v2(
                databasePath.string().c_str(),
                &p_openedDatabase,
                SQLITE_OPEN_READWRITE
                    | SQLITE_OPEN_FULLMUTEX
                    | SQLITE_OPEN_EXRESCODE
                    | SQLITE_OPEN_NOFOLLOW,
                nullptr
            );
            m_database.reset(p_openedDatabase);
            REQUIRE(opened == SQLITE_OK);
            REQUIRE(m_database != nullptr);
        }

        auto execute(std::string_view sql) -> void
        {
            REQUIRE(
                sqlite3_exec(
                    m_database.get(),
                    sql.data(),
                    nullptr,
                    nullptr,
                    nullptr
                ) == SQLITE_OK
            );
        }

        [[nodiscard]]
        auto refuses(std::string_view sql) -> bool
        {
            return sqlite3_exec(
                m_database.get(),
                sql.data(),
                nullptr,
                nullptr,
                nullptr
            ) != SQLITE_OK;
        }

        [[nodiscard]]
        auto readRows(
            std::string_view sql
        ) const -> std::vector<std::vector<std::string>>
        {
            REQUIRE(std::cmp_less_equal(
                sql.size(),
                std::numeric_limits<int>::max()
            ));
            // SAFETY: The checked bound above makes this narrowing exact.
            auto const sqlSize = static_cast<int>(sql.size());
            sqlite3_stmt* p_preparedStatement{};
            auto const prepared = sqlite3_prepare_v3(
                m_database.get(),
                sql.data(),
                sqlSize,
                SQLITE_PREPARE_PERSISTENT,
                &p_preparedStatement,
                nullptr
            );
            auto statement = std::unique_ptr<sqlite3_stmt, SqliteFinalize>{
                p_preparedStatement,
            };
            REQUIRE(prepared == SQLITE_OK);
            REQUIRE(statement != nullptr);

            auto rows = std::vector<std::vector<std::string>>{};
            auto step = sqlite3_step(statement.get());
            while (step == SQLITE_ROW)
            {
                auto row = std::vector<std::string>{};
                auto const columns = sqlite3_column_count(statement.get());
                REQUIRE(columns >= 0);
                // SAFETY: sqlite3_column_count returned a non-negative count.
                row.reserve(static_cast<std::size_t>(columns));
                for (auto column = 0; column < columns; ++column)
                {
                    auto const* p_text = sqlite3_column_text(
                        statement.get(),
                        column
                    );
                    auto const size = sqlite3_column_bytes(
                        statement.get(),
                        column
                    );
                    REQUIRE(p_text != nullptr);
                    REQUIRE(size >= 0);
                    // SAFETY: SQLite owns this buffer until the next step,
                    // and sqlite3_column_bytes bounds the same column value.
                    UF_UNSAFE_BUFFER_BEGIN
                    auto const text = std::span{
                        p_text,
                        static_cast<std::size_t>(size),
                    };
                    UF_UNSAFE_BUFFER_END
                    row.emplace_back(text.begin(), text.end());
                }
                rows.emplace_back(std::move(row));
                step = sqlite3_step(statement.get());
            }
            REQUIRE(step == SQLITE_DONE);
            return rows;
        }
    };
}
