#pragma once

#include "manifest.hpp"
#include "project-plugin.hpp"
#include "tool-invocation.hpp"

#include <script/scoped-tool-program.hpp>

#include <json/value.hpp>

#include <core/error/result.hpp>
#include <core/safety/annotations.hpp>

#include <domain/content-hash.hpp>

#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uf::operator_runtime
{
    // One loaded Project registration generation: the tool closure compiled on
    // the scoped program type, admitted whole or not at all.
    //
    // There is one closure because there is one thing a Project runs. The
    // framework asks a Project to answer calls of the Tools it declared; it
    // asks it for nothing else, and it reads nothing a Project's answer means.
    //
    // Holding one is proof of both halves of the export join. The generation
    // STATED what the closure exports; the loader proved that statement against
    // what it should be -- exactly the binding table's union -- and the Luau
    // bridge then proved the same statement against the closure's actual
    // exports when it compiled it. Neither proof can stand in for the other:
    // the first catches a document whose declared contract and stated code
    // disagree, the second catches stated code the shipped bytes do not honour.
    //
    // It is also proof of the declaration join: every Tool the pinned
    // declarations name is bound to an entry, and every binding names a Tool
    // those declarations carry.
    class ProjectGenerationHandle final
    {
        class State;

        friend class ProjectGenerationRegistrar;

        std::shared_ptr<State const> m_state;

        explicit ProjectGenerationHandle(
            std::shared_ptr<State const> p_state
        ) noexcept;

    public:
        ProjectGenerationHandle(ProjectGenerationHandle const&) noexcept = default;
        ProjectGenerationHandle(ProjectGenerationHandle&&) noexcept      = default;
        auto operator=(ProjectGenerationHandle const&) noexcept
            -> ProjectGenerationHandle& = default;
        auto operator=(ProjectGenerationHandle&&) noexcept
            -> ProjectGenerationHandle& = default;
        ~ProjectGenerationHandle() = default;

        [[nodiscard]] auto pluginId() const -> std::string;
        [[nodiscard]] auto projectRegistrationHash() const -> ContentHash;

        [[nodiscard]] auto toolModuleManifestHash() const -> ContentHash;

        // The Framework catalog this generation's scoped facades were pinned
        // to, and the scoped environment the tool closure was compiled under.
        // Both are exposed because a durable row must record which code
        // answered a call, and neither is derivable from the generation alone.
        [[nodiscard]] auto frameworkToolCatalogHash() const -> ContentHash;
        [[nodiscard]] auto environmentIdentity() const -> ContentHash;

        // The join this generation was admitted on, and the declaration
        // authority behind it. The table is what the tool closure was compiled
        // over; the declarations are handed out whole rather than projected,
        // because the bounds one call is admitted under are read from them per
        // call and never from anything the compiler saw.
        [[nodiscard]]
        auto bindingTable() const noexcept UF_LIFETIME_BOUND
            -> ProjectToolBindingTable const&;

        [[nodiscard]]
        auto catalog() const noexcept UF_LIFETIME_BOUND
            -> ProjectToolCatalogSchemaOwner const&;

        // Run the entry this generation bound to `toolName`, in one fresh
        // scoped VM, with the run's issuing coordinate carried in `request`. A
        // name this generation never bound is a refusal rather than an
        // absence.
        [[nodiscard]]
        auto invokeBoundTool(
            std::string_view toolName,
            json::Value const& canonicalArguments,
            script::ScopedRunRequest const& request
        ) const -> Result<json::Value>;
    };

    // Startup-only exact registry, one entry per (plugin id, registration
    // root). A registration root is a generation, so re-registering the same
    // root is refused rather than recompiled.
    class ProjectGenerationRegistrar final
    {
    public:
        // The closure as it is offered to the registrar: the entry module its
        // manifest digest is taken over, and the exact module blobs of the
        // closed graph beneath it.
        //
        // The resource closure is deliberately not in here. A registration
        // pins ONE resource closure, so a per-closure resource list would be
        // two answers to which bytes this project registered.
        struct ClosureModules final
        {
            std::string                    entryModule{};
            std::vector<ProjectModuleBlob> modules{};
        };

    private:
        std::map<
            std::pair<std::string, ContentHash>,
            ProjectGenerationHandle
        > m_generations{};

    public:
        // `catalog` must be the owner built over the exact Tool declaration
        // bytes this generation pinned, and `dispatchTool` is the native host
        // adapter the compiled tool closure reaches the Tool Runtime through.
        // It is bound once, at compile time, and must therefore carry no run
        // state: every run-scoped value travels in the ScopedRunRequest of the
        // invoke that is executing.
        //
        // The closure's declared export set is not derived here, and it is not
        // observed by running the module. The generation states it; this
        // function joins that statement against what the binding table says it
        // should be, and hands the statement itself to the bridge as the set to
        // admit the closure against.
        [[nodiscard]]
        auto registerGeneration(
            VerifiedProjectGeneration const& generation,
            ProjectToolCatalogSchemaOwner catalog,
            ClosureModules toolClosure,
            std::vector<ProjectResourceBlob> exactResources,
            script::ToolRuntimeDispatch dispatchTool
        ) -> Result<ProjectGenerationHandle>;

        [[nodiscard]]
        auto findExact(
            std::string const& pluginId,
            ContentHash projectRegistrationHash
        ) const -> Result<ProjectGenerationHandle>;
    };
} // namespace uf::operator_runtime
