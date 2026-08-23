#pragma once

#include "manifest.hpp"

#include <script/pure-data-program.hpp>

#include <json/value.hpp>

#include <core/error/result.hpp>
#include <core/safety/annotations.hpp>
#include <core/types/integer.hpp>

#include <domain/content-hash.hpp>

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uf::operator_runtime
{
    // Immutable exact JCS with no schema authority. It proves canonical bytes
    // and their content hash only; it makes no claim about what those bytes
    // mean, and there is nothing here that could: the framework records a
    // Project's bytes and their digest and does not interpret them.
    //
    // It carries the value those bytes denote beside them, because parseExact
    // had to build one to prove the bytes canonical and a consumer would
    // otherwise parse the same document a second time.
    class CanonicalJson final
    {
        ContentHash m_contentHash;
        std::string m_bytes;
        json::Value m_value;

        CanonicalJson(
            ContentHash contentHash,
            std::string bytes,
            json::Value value
        );

    public:
        [[nodiscard]]
        static auto parseExact(std::string exactJcs) -> Result<CanonicalJson>;

        [[nodiscard]] auto contentHash() const -> ContentHash;

        [[nodiscard]]
        auto bytes() const noexcept UF_LIFETIME_BOUND -> std::string const&;

        [[nodiscard]]
        auto value() const noexcept UF_LIFETIME_BOUND -> json::Value const&;

        auto operator==(CanonicalJson const&) const -> bool = default;
    };

    // One authored module of a Project closure, and one blob of the resource
    // closure a registration pinned, as they are handed to a loader.
    //
    // They live at namespace scope rather than inside the registrar because
    // they are the vocabulary of the loader layer and not of one loader: the
    // generation registrar, the manifest derivation and the deployment that
    // reads the bytes off disk all name them. Nesting them inside the registrar
    // would make every other consumer spell a loader's name to say "a module".
    struct ProjectModuleBlob final
    {
        std::string name{};
        std::string source{};
    };

    struct ProjectResourceBlob final
    {
        ProjectResourceKind kind{ProjectResourceKind::Json};
        std::string         name{};
        std::string         bytes{};
    };

    [[nodiscard]]
    auto derivePluginModuleManifestHash(
        std::string_view entryModule,
        std::span<ProjectModuleBlob const> modules
    ) -> Result<ContentHash>;

    [[nodiscard]]
    auto validateProjectResourceClosure(
        std::span<ProjectResourceBlob const> resources
    ) -> Status;

    // The exact Project resource closure a registration pinned, held to that
    // registration blob by blob -- name, kind, size and sha256 -- and returned
    // in the registration's own order, ready for a program to be compiled over.
    //
    // A registration pins ONE resource closure, so this is one function and not
    // one per caller. Two copies of this check would be two answers to "which
    // bytes did this project register", and only one of them would be inside
    // project_registration_hash.
    //
    // It takes the pinned rows rather than a registration so that the check
    // names the rows it judges and not the document that happens to carry
    // them. `pinnedResources` is a call-scoped borrow of the document's own
    // rows; nothing here retains it.
    [[nodiscard]]
    auto verifyProjectResourceClosure(
        std::span<ProjectResource const> pinnedResources,
        std::vector<ProjectResourceBlob> exactResources
    ) -> Result<std::vector<script::PureDataProgram::Resource>>;

    [[nodiscard]]
    auto currentProjectPluginEnvironmentMaterial() -> Result<std::string>;

    [[nodiscard]] auto currentProjectPluginEnvironmentHash() -> Result<ContentHash>;
} // namespace uf::operator_runtime
