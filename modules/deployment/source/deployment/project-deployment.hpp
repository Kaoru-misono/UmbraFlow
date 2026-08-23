#pragma once

#include <operator/effective-plan.hpp>
#include <operator/project-observation.hpp>
#include <operator/project-plugin.hpp>
#include <operator/ledger.hpp>
#include <operator/tool-invocation.hpp>
#include <operator/tool-runtime.hpp>

#include <core/error/result.hpp>

#include <domain/content-hash.hpp>

#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace uf::deployment
{
    // One observed-instance identity schema a deployment declares inline: the
    // name the framework keys the compiled authority on, and the exact
    // canonical bytes of the JSON Schema document that judges a semantic
    // identity basis.
    //
    // Views, not owned strings: create() compiles the bytes and retains
    // nothing, so they need only outlive the call.
    struct ProjectIdentitySchemaSource final
    {
        std::string_view name{};
        std::string_view schema{};
    };

    // Everything one deployment declares for one ProjectRegistration, as exact
    // canonical bytes. Nothing here is a path: the declaration is one document
    // and its declarative members are inline, so what reaches this seam is the
    // bytes themselves.
    //
    // Views, not owned strings: create() reads every one of them and retains
    // nothing, so the bytes need only outlive the call.
    struct ProjectDeploymentSources final
    {
        // The registration's plugin id. Every Tool this deployment declares is
        // named inside it, and a Tool naming another registrant's namespace is
        // refused here: a declaration that answered for whichever registration
        // presented it would be a declaration no registration owns.
        std::string_view pluginId{};

        // The exact canonical JCS of the deployment's `tools` array. It is the
        // whole Tool declaration -- there is no catalog document and no catalog
        // file -- and its sha256 is the registration's tool_catalog_hash.
        std::string_view tools{};

        std::span<ProjectIdentitySchemaSource const> observedInstanceIdentitySchemas{};
    };

    // The whole of the Tool Runtime protocol this release implements, rendered
    // canonically: the call-state vocabulary and the conclusions that write it,
    // the exact identity preimages, the durable record's stored DDL, the
    // canonical-form contract every one of those bytes is judged under, and the
    // published framework schema that decides what a Tool declaration is.
    //
    // It lives in this module and not in operator because of the last member.
    // The project directory schema is a framework format, but its bytes are
    // compiled here out of the framework schema catalog, and operator does not
    // depend on this module and must not -- an Operator that reached into a
    // deployment for its own validators would be the deployment. So the
    // assembly happens at the lowest layer that can see both halves, and each
    // half is rendered by the module that owns it.
    //
    // tool_runtime_protocol_identity is the SHA-256 of these bytes. The
    // recording incarnation writes it into the tool_runs row and a resuming one
    // re-derives it from its own release; the equality between the two is the
    // join, and it is a real one because the durable row is independent of the
    // binary now reading it.
    [[nodiscard]]
    auto currentToolRuntimeProtocolMaterial() -> Result<std::string>;

    [[nodiscard]]
    auto currentToolRuntimeProtocolIdentity() -> Result<ContentHash>;

    // The schema-bearing validators one ProjectRegistration's authorities are
    // built from. Immutable and copyable: each accessor hands out a
    // std::function that keeps this state alive, so an authority outlives the
    // ProjectDeployment it was taken from.
    class ProjectDeployment final
    {
        class State;

        std::shared_ptr<State const> m_state;

        explicit ProjectDeployment(std::shared_ptr<State const> p_state) noexcept;

    public:
        ProjectDeployment(ProjectDeployment const&) noexcept = default;
        ProjectDeployment(ProjectDeployment&&) noexcept = default;
        auto operator=(ProjectDeployment const&) noexcept -> ProjectDeployment& = default;
        auto operator=(ProjectDeployment&&) noexcept -> ProjectDeployment& = default;
        ~ProjectDeployment() = default;

        // Reads the Tool declarations and compiles every schema they and the
        // identity set state inline. It refuses a schema this evaluator cannot
        // apply and a Tool named outside the registrant's namespace -- at
        // startup, where a deployment builds its authorities, rather than years
        // later when a document reaches the hole.
        //
        // It judges the declaration's SHAPE against the one published statement
        // of it, `schema/umbraflow-project-v3.schema.json`, compiled out of the
        // framework schema catalog. There is no second, narrower reading of a
        // Tool entry anywhere in this module.
        [[nodiscard]]
        static auto create(ProjectDeploymentSources const& sources)
            -> Result<ProjectDeployment>;

        // What this deployment declares under one Tool name, or nothing at all.
        //
        // toolCatalogReader() below hands the whole set to the Operator's own
        // owner, which is where a session reads it from. This answers for one
        // name without building an owner at all, which is what lets a document
        // that names tools without calling them -- a conformance vocabulary --
        // and this deployment's declarations be held to each other where both
        // were written.
        [[nodiscard]]
        auto carriedTool(std::string_view name) const
            -> std::optional<operator_runtime::ToolDescriptor>;

        // The declarations are read once and the arguments of each call are
        // judged per call, so these are two callbacks rather than one. See
        // ToolCatalogReader in operator/tool-invocation.hpp for why the split
        // is what keeps the offer side and the accept side answering from one
        // stored declaration.
        [[nodiscard]]
        auto toolCatalogReader() const -> operator_runtime::ToolCatalogReader;

        // A Tool whose argument_schema is the string `unchecked` gets no
        // argument enforcement, which is the Project declining a guard rather
        // than the framework skipping one. The framework still records the
        // exact bytes it passed, their digest and their coordinates.
        [[nodiscard]]
        auto toolArgumentValidator() const
            -> operator_runtime::ToolArgumentValidator;

        // The identity schemas this deployment compiled, as the bindings the
        // ObservedInstanceIdentitySchemas authority is built from. Each
        // validator keeps this state alive, so an authority built from the
        // result outlives the ProjectDeployment it was taken from.
        [[nodiscard]]
        auto observedIdentitySchemas() const
            -> std::vector<operator_runtime::ObservedInstanceIdentitySchema>;
    };
}
