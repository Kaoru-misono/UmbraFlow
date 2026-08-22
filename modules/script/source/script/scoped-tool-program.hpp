#pragma once

#include <script/pure-data-program.hpp>

#include <json/value.hpp>

#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <domain/content-hash.hpp>

#include <cstddef>
#include <functional>
#include <memory>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace uf::script
{
    // Where one scoped Tool call sits in the recorded call tree. Both halves are
    // assigned by the C++ seam and neither is reachable from script: a script
    // that could name its own ordinal could alias another call's position and
    // inherit its recorded outcome.
    //
    // No in-class initializer for the parent: ContentHash has no default state,
    // and that is the point. A coordinate names a durable row or it does not
    // exist.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
    struct ToolCallCoordinate final
    {
        // The durable parent position this run's calls are numbered under, by
        // the identity the Operator recorded that row under. It is never
        // absent and never a run-local number: a root run is itself a
        // positioned call, and a handler run is numbered under the position of
        // the call it implements. One Tool Runtime is shared by every run of a
        // registration, so this is what tells it WHICH run a call belongs to,
        // and nothing else it receives could.
        //
        // The Operator owns what the value means; this module only carries it
        // through, because a script module that could name a root request
        // identity would be a script module that could mint one.
        ContentHash parentPosition;

        // The issuing context's own monotone child index, starting at 1.
        // Per issuing context and never global: a replayed child costs its
        // parent exactly one increment however large the subtree it replaced,
        // so a context's numbering depends only on that context's behaviour.
        uint64 childIndex{0};
    };

    // The one native seam a ScopedToolProgram reaches: canonical JSON in,
    // canonical JSON out, at a coordinate the seam assigned. This module owns
    // the type and links no Operator, so the implementation that drives
    // ToolRuntimeExecutor is injected by whoever composes the two.
    //
    // Synchronous and blocking by contract: it runs to completion and returns a
    // decoded result. There is no yield, no coroutine suspension and no callback
    // back into the VM, because deterministic restart-replay is the only
    // suspension mechanism this architecture has.
    //
    // A returned FAILURE is terminal for the run. It records the failure and
    // destroys the VM without resuming the script, so no pcall can swallow it
    // and a divergence cannot be converted into ordinary control flow. A Tool
    // that ran and failed is therefore not a failure here: its classification
    // travels inside the JSON value it answers with.
    //
    // Lifetime contract: a ScopedToolProgram copies and owns this callable for
    // as long as any run it started is alive, so the callable must own
    // everything it reaches and must not capture a reference or a bare `this`.
    using ToolRuntimeInvoke = std::function<
        Result<json::Value>(
            std::string_view toolName,
            json::Value const& arguments,
            ToolCallCoordinate const& coordinate,
            std::stop_token cancellation
        )
    >;

    // What one scoped run is started under.
    //
    // No in-class initializer for the parent, for ToolCallCoordinate's reason:
    // a request that could be default-constructed would be a run anchored on
    // nothing, and "anchored on nothing" is the absent-means-something reading
    // this type exists to make unspellable. A caller with no durable position
    // to name has no run to start, and the closure admission compile() performs
    // is exactly that caller: it builds no request at all.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
    struct ScopedRunRequest final
    {
        // The durable parent position this run's issuing context is anchored
        // on, by the identity the Operator recorded that row under. Every Tool
        // call the run makes is numbered under it, and there is no absent
        // value: the Operator hands a root run the coordinate of the
        // root-positioned call the run implements, exactly as it hands a
        // handler run the position of its own call.
        ContentHash parentPosition;

        // Hard cancellation. Armed on the Luau interrupt as well as handed to
        // every Tool call, because a script can spin in pure computation without
        // ever reaching one. A stop is teardown rather than unwind: the VM dies
        // without resuming the script, deliberately indistinguishable from a
        // crash, and the durable machinery covers it.
        std::stop_token cancellation{};
    };

    // The third program type. It shares PureDataProgram's closed-graph
    // compilation, host-owned require resolver, per-invoke fresh quota-bound VM,
    // resource closure and deep-freeze discipline, and adds exactly one native
    // seam: the Tool Runtime call.
    //
    // The scoped module catalog is a property of THIS TYPE and not of a VM boot
    // or a constructor flag, because a pure reducer's inability to load a scoped
    // module has to fail in the RESOLVER, naming the module it asked for. Two
    // genuinely different contracts get two types; a capability-set parameter
    // would make purity a value, and a program in a signature would stop telling
    // the reader whether the thing in hand can reach the world.
    //
    // The four scoped modules are Luau sources in the Framework bundle, exactly
    // as the pure Framework modules are. compile() refuses a bundle that does
    // not carry all four under their exact names, and hands each of them, and
    // nothing else, the private capability table as its chunk argument.
    class ScopedToolProgram final
    {
    public:
        // Bounded like every other name this boundary admits, and bounded per
        // context so the monotone child index cannot run away inside one run.
        static constexpr auto k_maximumToolNameBytes        = std::size_t{128U};
        static constexpr auto k_maximumToolNameSegments     = std::size_t{8U};
        static constexpr auto k_maximumToolNameSegmentBytes = std::size_t{64U};
        static constexpr auto k_maximumToolCallsPerContext  = uint64{1024U};

    private:
        class State;

        std::shared_ptr<State const> m_state;

        explicit ScopedToolProgram(std::shared_ptr<State const> p_state) noexcept;

    public:
        ScopedToolProgram(ScopedToolProgram const&) noexcept = default;
        ScopedToolProgram(ScopedToolProgram&&) noexcept      = default;
        auto operator=(ScopedToolProgram const&) noexcept -> ScopedToolProgram& = default;
        auto operator=(ScopedToolProgram&&) noexcept -> ScopedToolProgram&      = default;
        ~ScopedToolProgram() = default;

        // The exact Framework modules this program type admits beyond the pure
        // ones, in the order its environment identity states them. Published as
        // data for the same reason the pure global whitelist is: the set is
        // otherwise unobservable from outside a VM, so a test can bind it only
        // by comparing it here.
        [[nodiscard]]
        static auto scopedModuleNames() -> std::span<std::string_view const>;

        // `frameworkModules` must carry every name scopedModuleNames() states,
        // and may carry pure Framework modules besides. `invokeTool` is the one
        // native seam; an empty callable is refused, because a scoped program
        // with no Tool Runtime is a pure program wearing the wrong type.
        //
        // The pinned Tool catalog the scoped facades read is a Framework
        // resource and belongs in `frameworkResources`, whose names must all sit
        // inside the reserved `umbraflow.` namespace that `resources` is refused
        // for. The catalog's own name is the Framework bundle's to state; this
        // module reserves the class and never names one member of it.
        [[nodiscard]]
        static auto compile(
            std::string_view pluginId,
            std::string_view entryModule,
            std::vector<PureDataProgram::Module> modules,
            std::span<std::string_view const> entryPoints,
            std::vector<PureDataProgram::Resource> resources,
            std::span<FrameworkModule const> frameworkModules,
            std::vector<PureDataProgram::Resource> frameworkResources,
            ToolRuntimeInvoke invokeTool
        ) -> Result<ScopedToolProgram>;

        // One run in one fresh VM. Every Tool call it issues is numbered from 1
        // under `request.parentPosition`, and a second invoke() starts a second
        // issuing context whose numbering starts again at 1 -- on a re-entry
        // after a crash exactly as on a first entry, because the recorded
        // ordinals are re-derived by re-executing rather than resumed from a
        // stored counter.
        [[nodiscard]]
        auto invoke(
            std::string_view entryPoint,
            json::Value const& immutableInput,
            ScopedRunRequest const& request
        ) const -> Result<json::Value>;
    };

    // The exact scoped environment bytes scopedToolEnvironmentHash is taken
    // over: everything the pure environment attests to, plus the scoped module
    // catalog, the Tool Runtime facade contract and the ceilings only this
    // program type has. Published for the same reason the pure material is, and
    // distinct from pluginEnvironmentMaterial() by construction: a build that
    // moved one scoped module name or changed what invoke answers with must move
    // this digest and must not move that one.
    [[nodiscard]]
    auto scopedToolEnvironmentMaterial() -> std::string;

    [[nodiscard]]
    auto scopedToolEnvironmentHash() -> Result<ContentHash>;
} // namespace uf::script
