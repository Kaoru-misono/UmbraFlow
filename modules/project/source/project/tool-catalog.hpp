#pragma once

#include <core/error/result.hpp>

#include <domain/content-hash.hpp>

#include <json/value.hpp>

#include <operator/tool-descriptor.hpp>

#include <string>
#include <vector>

namespace uf::project
{
    struct DeclaredTool final
    {
        std::string name{};

        // The definitions of this project's tool precondition schema that
        // judge one call of this tool. Both are required and neither has an
        // absent spelling: a tool that answers nothing still answers a value,
        // and a declaration that could omit its result schema would be a tool
        // whose answer nothing is entitled to judge.
        //
        // They are names rather than inline documents for the same reason the
        // effect bounds name a payload digest: the documents' bytes are pinned
        // by project_tool_precondition_schema_hash, and the names are pinned by
        // tool_catalog_hash, so neither can move without moving a registration
        // digest.
        std::string argumentSchema{};
        std::string resultSchema{};

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

    // The inverse of generateToolCatalog, for the reader a declared tool
    // catalog source is: one document judged member by member back into the
    // declaration the generator renders. The shape authority is the deployment
    // loader's embedded schema, which this module cannot compile, so the rules
    // that document states are stated here in C++ -- closed objects, required
    // members, the wire names, the numeric bounds -- and whatever the schema
    // leaves to the generator (duplicate tools, empty names, a bound set that
    // does not close over the effect payload hashes) is left to
    // generateToolCatalog, which refuses it one call later.
    [[nodiscard]]
    auto parseToolCatalogDeclaration(
        json::Value const& document
    ) -> Result<ToolCatalogDeclaration>;
}
