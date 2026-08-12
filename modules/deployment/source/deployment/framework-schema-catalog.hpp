#pragma once

#include <span>
#include <string_view>

namespace uf::deployment
{
    struct FrameworkSchemaDocument final
    {
        std::string_view identity{};
        std::string_view relativePath{};
        std::string_view sha256{};
        std::string_view exactBytes{};
    };

    // The returned documents are generated from schema/ at build time. Their
    // views name static storage in the deployment module.
    [[nodiscard]]
    auto frameworkSchemaCatalog() noexcept
        -> std::span<FrameworkSchemaDocument const>;
}
