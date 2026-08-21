#pragma once

#include <script/engine.hpp>

#include <json/value.hpp>

#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <domain/content-hash.hpp>

#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace uf::script
{
    // Immutable Luau bytecode for one closed pure module graph. Compilation and
    // exact entry-export validation happen once in compile(); invoke() loads
    // the graph into a fresh quota-bound VM and passes one decoded JSON value
    // in and one decoded JSON value out. A host-owned require resolves canonical
    // Project logical names plus exact reserved @umbraflow/ Framework names,
    // caches one module value per VM, and never observes a filesystem or package
    // search path. Framework modules may import only other reserved Framework
    // modules, and every reachable table in their exports is deep-frozen.
    //
    // Typed resource readers expose only the immutable resource closure passed
    // to compile(). JSON is decoded and frozen, UTF-8 is admitted before the
    // program is built, and bytes remain a Luau byte string. No host installer
    // or native capability seam is part of this API.
    class PureDataProgram final
    {
    public:
        static constexpr auto k_maximumModuleCount = std::size_t{64U};
        static constexpr auto k_maximumModuleSourceBytes =
            std::size_t{256U} * 1024U;
        static constexpr auto k_maximumModuleClosureSourceBytes =
            std::size_t{4U} * 1024U * 1024U;
        static constexpr auto k_maximumModuleBytecodeBytes =
            std::size_t{1024U} * 1024U;
        static constexpr auto k_maximumModuleClosureBytecodeBytes =
            std::size_t{16U} * 1024U * 1024U;
        static constexpr auto k_maximumResourceCount = std::size_t{64U};
        static constexpr auto k_maximumResourceBytes =
            std::size_t{4U} * 1024U * 1024U;
        static constexpr auto k_maximumResourceClosureBytes =
            std::size_t{16U} * 1024U * 1024U;
        static constexpr auto k_memoryQuotaBytes = std::size_t{16U} * 1024U * 1024U;

        struct Module final
        {
            std::string name{};
            std::string source{};
        };

        enum class ResourceKind : uint8
        {
            Json,
            Utf8,
            Bytes,
        };

        struct Resource final
        {
            ResourceKind kind{ResourceKind::Json};
            std::string  name{};
            std::string  bytes{};
        };

    private:
        class State;

        std::shared_ptr<State const> m_state;

        explicit PureDataProgram(std::shared_ptr<State const> p_state) noexcept;

    public:
        PureDataProgram(PureDataProgram const&) noexcept = default;
        PureDataProgram(PureDataProgram&&) noexcept = default;
        auto operator=(PureDataProgram const&) noexcept -> PureDataProgram& = default;
        auto operator=(PureDataProgram&&) noexcept -> PureDataProgram& = default;
        ~PureDataProgram() = default;

        // Admission without compilation, shared by offline builders and
        // deployment loaders so an earlier boundary cannot accept a closure
        // this runtime later refuses for canonical names, kinds, encoding, or
        // fixed source/resource quotas.
        [[nodiscard]]
        static auto validateModuleClosure(
            std::string_view entryModule,
            std::span<Module const> modules
        ) -> Status;

        [[nodiscard]]
        static auto validateResourceClosure(
            std::span<Resource const> resources
        ) -> Status;

        [[nodiscard]]
        static auto compile(
            std::string_view pluginId,
            std::string_view entryModule,
            std::vector<Module> modules,
            std::span<std::string_view const> entryPoints,
            std::vector<Resource> resources,
            std::span<FrameworkModule const> frameworkModules = {}
        ) -> Result<PureDataProgram>;

        [[nodiscard]]
        auto invoke(
            std::string_view entryPoint,
            json::Value const& immutableInput
        ) const
            -> Result<json::Value>;
    };

    // The exact Luau globals a plugin environment publishes, in the order the
    // whitelist states them. Published as data because the size of this set is
    // a fact other documents quote and the environment is otherwise
    // unobservable from outside a VM: a plugin cannot enumerate its own globals
    // (no _G, no getfenv), so a test can bind the set only by comparing it
    // here.
    [[nodiscard]]
    auto pureEnvironmentGlobals() -> std::span<std::string_view const>;

    // The exact lower-level pure-data environment bytes
    // pluginEnvironmentHash is taken over. ProjectPlugin registration wraps
    // these bytes with its exact reserved Framework SDK before taking the
    // outward plugin_environment_hash. Published for the same reason the
    // whitelist is: a test must be able to prove which member moved the digest.
    [[nodiscard]]
    auto pluginEnvironmentMaterial() -> std::string;

    // The lower-level pure-data environment identity: the trusted Luau bridge,
    // pinned Luau implementation, global whitelist, frozen tables and
    // versioned contracts. ProjectPlugin registration uses the wrapper identity
    // exposed by currentProjectPluginEnvironmentHash(), which also binds the
    // reserved Framework SDK.
    [[nodiscard]]
    auto pluginEnvironmentHash() -> Result<ContentHash>;
} // namespace uf::script
