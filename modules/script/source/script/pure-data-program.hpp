#pragma once

#include <json/value.hpp>

#include <core/error/result.hpp>

#include <domain/content-hash.hpp>

#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace uf::script
{
    // Immutable Luau bytecode for one pure, data-only module. Compilation and
    // exact export validation happen once in compile(); invoke() loads those
    // bytes into a fresh quota-bound VM and passes one decoded JSON value in
    // and one decoded JSON value out. The boundary is a value rather than
    // bytes: the host has already parsed the document, so a plugin that
    // received bytes would re-derive structure the caller already holds, and a
    // plugin that returned bytes could emit a non-canonical serialization of a
    // value the host is about to canonicalize anyway. A frozen artifact.read
    // (name) reader lazily copies only registered immutable blobs into that VM.
    // No host installer or native capability seam is part of this API.
    class PureDataProgram final
    {
        class State;

        std::shared_ptr<State const> m_state;

        explicit PureDataProgram(std::shared_ptr<State const> p_state) noexcept;

    public:
        struct Artifact final
        {
            std::string name{};
            std::string bytes{};
        };

        PureDataProgram(PureDataProgram const&) noexcept = default;
        PureDataProgram(PureDataProgram&&) noexcept = default;
        auto operator=(PureDataProgram const&) noexcept -> PureDataProgram& = default;
        auto operator=(PureDataProgram&&) noexcept -> PureDataProgram& = default;
        ~PureDataProgram() = default;

        [[nodiscard]]
        static auto compile(std::string_view moduleId,
                            std::string_view source,
                            std::span<std::string_view const> entryPoints,
                            std::vector<Artifact> artifacts) -> Result<PureDataProgram>;

        [[nodiscard]]
        auto invoke(std::string_view entryPoint, json::Value const& immutableInput) const
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

    // The identity of the plugin environment this build runs: the trusted Luau
    // bridge that wraps every plugin call, the global whitelist above, and the
    // frozen tables published beside it. It belongs in a SessionManifest for
    // the same reason the protocol schema hashes do -- a framework upgrade that
    // changed any of them would change what a plugin does under a session
    // manifest that had not moved.
    [[nodiscard]]
    auto pluginEnvironmentHash() -> Result<ContentHash>;
} // namespace uf::script
