#pragma once

#include <optional>
#include <span>
#include <string_view>

namespace uf::framework_schema
{
    struct FrameworkSchemaDocument final
    {
        std::string_view identity{};
        std::string_view relativePath{};
        std::string_view sha256{};
        std::string_view exactBytes{};
    };

    // The returned views name static storage generated from schema/ at build
    // time. The catalog is sorted by relative path.
    [[nodiscard]]
    auto frameworkSchemaCatalog() noexcept
        -> std::span<FrameworkSchemaDocument const>;

    // The returned document owns no bytes; every view names the catalog's
    // process-lifetime static storage.
    [[nodiscard]]
    auto findFrameworkSchema(std::string_view relativePath) noexcept
        -> std::optional<FrameworkSchemaDocument>;
}
