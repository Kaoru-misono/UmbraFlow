#pragma once

#include "manifest.hpp"
#include "project-plugin.hpp"

#include <script/pure-data-program.hpp>
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
    // One loaded two-closure Project registration generation: the reducer
    // compiled on the pure program type and the tool closure compiled on the
    // scoped program type, admitted together or not at all.
    //
    // Two types rather than two spellings. Reduction runs only on the pure
    // type, whose resolver refuses every scoped module by name, and dispatch
    // runs only on the scoped type, which is the only one holding a Tool
    // Runtime seam. Nothing here selects between them: there is no entry name
    // in both contracts and no value that decides where a name lives.
    //
    // Holding one is proof of both halves of the export join. The generation
    // STATED what each closure exports; the loader proved each statement
    // against what it should be -- exactly `reduce` for the reducer, exactly
    // the binding table's union for the tool closure -- and the Luau bridge
    // then proved each statement against the closure's actual exports when it
    // compiled it. Neither proof can stand in for the other: the first catches
    // a document whose declared contract and stated code disagree, the second
    // catches stated code the shipped bytes do not honour.
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

        // Two closures, two manifest digests, answered apart. A generation
        // whose two closures were one digest would be a generation that could
        // not move one closure's code without moving the other's identity.
        [[nodiscard]] auto reducerModuleManifestHash() const -> ContentHash;
        [[nodiscard]] auto toolModuleManifestHash() const -> ContentHash;

        // The scoped environment the tool closure was compiled under. It is
        // exposed because a durable row must record which code answered a
        // call, and it is not derivable from the generation alone.
        [[nodiscard]] auto environmentIdentity() const -> ContentHash;

        // Run the generation's fold, in one fresh pure VM. It stops at the
        // program's own answer: no schema owner judges the value here, because
        // the document authorities a reduced baseline is validated against
        // belong above this seam.
        [[nodiscard]]
        auto reduce(json::Value const& immutableInput) const
            -> Result<json::Value>;

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
        // One closure as it is offered to the registrar: the entry module its
        // manifest digest is taken over, and the exact module blobs of the
        // closed graph beneath it.
        //
        // The resource closure is deliberately not in here. A registration
        // pins ONE resource closure and both program types read it, so a
        // per-closure resource list would be two answers to which bytes this
        // project registered.
        struct ClosureModules final
        {
            std::string                                     entryModule{};
            std::vector<ProjectPluginRegistrar::ModuleBlob> modules{};
        };

    private:
        std::map<
            std::pair<std::string, ContentHash>,
            ProjectGenerationHandle
        > m_generations{};

    public:
        // `invokeTool` is the one native seam the compiled tool closure reaches
        // the Tool Runtime through. It is bound once, at compile time, and must
        // therefore carry no run state: every run-scoped value travels in the
        // ScopedRunRequest of the invoke that is executing.
        //
        // Neither closure's declared export set is derived here, and neither is
        // observed by running the module. The generation states both; this
        // function joins each statement against what the contract says it
        // should be, and hands the statement itself to the bridge as the set to
        // admit the closure against.
        [[nodiscard]]
        auto registerGeneration(
            VerifiedProjectGeneration const& generation,
            ClosureModules reducerClosure,
            ClosureModules toolClosure,
            std::vector<ProjectPluginRegistrar::ResourceBlob> exactResources,
            script::ToolRuntimeInvoke invokeTool
        ) -> Result<ProjectGenerationHandle>;

        [[nodiscard]]
        auto findExact(
            std::string const& pluginId,
            ContentHash projectRegistrationHash
        ) const -> Result<ProjectGenerationHandle>;
    };
} // namespace uf::operator_runtime
