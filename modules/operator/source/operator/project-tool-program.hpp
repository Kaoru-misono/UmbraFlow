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
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uf::operator_runtime
{
    // The scoped execution environment a loaded Project generation runs in:
    // everything script::scopedToolEnvironmentMaterial() attests to -- the
    // trusted bridge, the compiler options, every ceiling, the scoped module
    // catalog and the Tool Runtime facade contract -- plus the exact source
    // bytes, depth and Project visibility of every Framework module the scoped
    // closure admits, and the reserved resource name the pinned Tool catalog is
    // read from.
    //
    // It is deliberately a second identity beside
    // currentProjectPluginEnvironmentMaterial() and not a replacement for it. A
    // registration's plugin_environment_hash pins the pure closure both program
    // types share, and this pins what only the scoped type adds; a build that
    // moved a scoped facade's bytes moves this and moves that one not at all.
    // Nothing folds the two into one digest, because a project that ships no
    // handler would then have its identity moved by facades it can never load.
    [[nodiscard]]
    auto currentScopedToolEnvironmentMaterial() -> Result<std::string>;

    [[nodiscard]] auto currentScopedToolEnvironmentHash() -> Result<ContentHash>;

    // One Project registration generation, joined once and compiled once.
    //
    // ONE program per generation, never one per bound entry. The registration
    // carries one plugin environment, singular; per-entry programs would need
    // per-entry environment identities with no home in that model, would be N
    // copies of one closed closure under N identities, and would buy no
    // isolation a fresh VM per invoke does not already deliver. The program's
    // entry points are exactly the binding table's union, and the per-entry
    // variance a Tool runs under -- its requested child Tool set, its ceilings,
    // its budgets -- lives in the entry's DESCRIPTOR, hashed inside
    // tool_catalog_hash and read per call through catalog(). None of it is a
    // compile input: budgets as compile inputs would make policy motion move
    // code identity.
    //
    // Holding one is proof that a registration, its pinned Tool Catalog, its
    // exact module and resource closure, its binding table, the scoped SDK
    // generation and the Tool Runtime generation were all bound to each other
    // before any run started.
    class ProjectToolProgramHandle final
    {
        class State;

        friend class ProjectToolProgramRegistrar;

        std::shared_ptr<State const> m_state;

        explicit ProjectToolProgramHandle(
            std::shared_ptr<State const> p_state
        ) noexcept;

    public:
        ProjectToolProgramHandle(ProjectToolProgramHandle const&) noexcept = default;
        ProjectToolProgramHandle(ProjectToolProgramHandle&&) noexcept      = default;
        auto operator=(ProjectToolProgramHandle const&) noexcept
            -> ProjectToolProgramHandle& = default;
        auto operator=(ProjectToolProgramHandle&&) noexcept
            -> ProjectToolProgramHandle& = default;
        ~ProjectToolProgramHandle() = default;

        [[nodiscard]] auto pluginId() const -> std::string;
        [[nodiscard]] auto projectRegistrationHash() const -> ContentHash;
        [[nodiscard]] auto toolCatalogHash() const -> ContentHash;

        // The Framework catalog this program's scoped facades were pinned to,
        // and the scoped environment the program was compiled under. Both are
        // exposed because a durable row must record which code answered a
        // call, and neither is derivable from the registration alone.
        [[nodiscard]] auto frameworkToolCatalogHash() const -> ContentHash;
        [[nodiscard]] auto environmentIdentity() const -> ContentHash;

        // The join this generation was admitted on. entryPoints() on it is the
        // exact set the one program was compiled with.
        [[nodiscard]]
        auto bindingTable() const noexcept UF_LIFETIME_BOUND
            -> ProjectToolBindingTable const&;

        // The declaration authority for every Tool this generation binds. It
        // is handed out whole rather than projected, because the bounds one
        // call is admitted under are read here, per call, and never from
        // anything the compiler saw.
        [[nodiscard]]
        auto catalog() const noexcept UF_LIFETIME_BOUND
            -> ProjectToolCatalogSchemaOwner const&;

        // Run the entry this registration bound to `toolName`, in one fresh
        // VM, with the run's issuing coordinate carried in `request`.
        //
        // This is the bottom of the loader and the top of the dispatcher, and
        // it deliberately stops here: it establishes no issuing context, spends
        // no budget, validates the answer against no result schema and writes
        // no durable row. Those are the dispatcher's, above this seam, and the
        // value it returns is the Tool's own answer exactly as the program
        // produced it.
        [[nodiscard]]
        auto invokeBoundTool(
            std::string_view toolName,
            json::Value const& canonicalArguments,
            script::ScopedRunRequest const& request
        ) const -> Result<json::Value>;

        // Whether the answer a bound entry produced is one this Tool's catalog
        // entry declared it could produce. The dispatcher runs it between the
        // program's answer and the terminal durable row, so a handler that
        // breaks its own published contract fails the call rather than
        // recording an outcome nothing can read.
        [[nodiscard]]
        auto validateToolResult(
            std::string_view toolName,
            std::string_view exactResultJcs
        ) const -> Status;
    };

    // Startup-only exact registry, one entry per (plugin id, registration
    // root). A registration root is a generation, so re-registering the same
    // root is refused rather than recompiled: two programs under one identity
    // would be two answers to "which code answers this Tool".
    class ProjectToolProgramRegistrar final
    {
        std::map<
            std::pair<std::string, ContentHash>,
            ProjectToolProgramHandle
        > m_programs{};

    public:
        // `catalog` must be the owner built over the exact Tool Catalog bytes
        // this registration pinned, and `invokeTool` is the one native seam the
        // compiled program reaches the Tool Runtime through. The seam is bound
        // once, at compile time, and must therefore carry no run state: every
        // run-scoped value travels in the ScopedRunRequest of the invoke that
        // is executing.
        //
        // `exportedEntryPoints` is what the Project's closure offers -- a fact
        // about its code, stated separately from the binding table, which is a
        // fact about its contract. The loader's whole job is to refuse a
        // disagreement between the two, so it cannot derive one from the other.
        // A statement here that the closure does not honour survives nothing:
        // the program is compiled over the binding table's union, and a closure
        // exporting anything else is refused at admission.
        //
        // `validateResults` judges what a bound entry answers with, and is
        // required for the reason the argument validator is: a generation whose
        // answers nothing judges is a generation whose result schemas are
        // decoration.
        [[nodiscard]]
        auto registerProject(
            VerifiedProjectRegistration const& registration,
            ProjectToolCatalogSchemaOwner catalog,
            std::string entryModule,
            std::vector<ProjectPluginRegistrar::ModuleBlob> exactModules,
            std::vector<ProjectPluginRegistrar::ResourceBlob> exactResources,
            std::span<std::string const> exportedEntryPoints,
            ToolResultValidator validateResults,
            script::ToolRuntimeInvoke invokeTool
        ) -> Result<ProjectToolProgramHandle>;

        [[nodiscard]]
        auto findExact(
            std::string const& pluginId,
            ContentHash projectRegistrationHash
        ) const -> Result<ProjectToolProgramHandle>;
    };
} // namespace uf::operator_runtime
