#pragma once

#include <core/error/result.hpp>

#include <domain/content-hash.hpp>

#include <operator/tool-descriptor.hpp>

#include <string>
#include <vector>

namespace uf::project
{
    struct DeclaredTool final
    {
        std::string                      name{};
        std::string                      argumentSchema{};
        operator_runtime::ToolDescriptor descriptor{};
    };

    // No in-class initializer for the hash: ContentHash has no default state.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
    struct ToolCatalogDeclaration final
    {
        std::string               comment{};
        std::string               pluginId{};
        ContentHash               toolPreconditionSchemaHash;
        std::vector<ContentHash>  effectPayloadSchemaHashes{};
        std::vector<DeclaredTool> tools{};
    };

    [[nodiscard]]
    auto generateToolCatalog(
        ToolCatalogDeclaration const& declaration
    ) -> Result<std::string>;
}
