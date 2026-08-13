#include "framework-schema-catalog.hpp"

#include <algorithm>
#include <ranges>

namespace uf::framework_schema
{
    auto findFrameworkSchema(std::string_view relativePath) noexcept
        -> std::optional<FrameworkSchemaDocument>
    {
        auto const catalog = frameworkSchemaCatalog();
        auto const found = std::ranges::lower_bound(
            catalog,
            relativePath,
            {},
            &FrameworkSchemaDocument::relativePath
        );
        if (found == catalog.end() || found->relativePath != relativePath)
        {
            return std::nullopt;
        }
        return *found;
    }
}
