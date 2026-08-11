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
    // value the host is about to canonicalize anyway.
    //
    // A frozen artifact.read(name) reader answers with the decoded, frozen
    // value of a registered artifact, for the same reason and one step
    // further: the environment publishes no JSON decoder and a consuming
    // project ships no C++, so bytes carrying JSON were a capability no plugin
    // could express. compile() parses every artifact once -- the bytes are
    // pinned by the artifact root hash, so their value is a fact about the
    // registration rather than about any one call -- and each call builds the
    // Luau value for an artifact at most once, so two reads answer with the
    // same object. No host installer or native capability seam is part of this
    // API.
    class PureDataProgram final
    {
        class State;

        std::shared_ptr<State const> m_state;

        explicit PureDataProgram(std::shared_ptr<State const> p_state) noexcept;

    public:
        // The exact bytes one artifact root pins. They are an input to
        // compile() and are not retained: what a call reaches is the value
        // they denote, and no plugin can observe the serialization it came
        // from.
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

    // The exact bytes pluginEnvironmentHash is taken over. Published for the
    // same reason the whitelist is: the preimage is otherwise readable only in
    // pure-data-program.cpp, and a test that cannot name a member of it can
    // say only that the digest moved, never that it moved for the right
    // reason.
    [[nodiscard]]
    auto pluginEnvironmentMaterial() -> std::string;

    // The identity of the plugin environment this build runs: the trusted Luau
    // bridge that wraps every plugin call, the global whitelist above, the
    // frozen tables published beside it, and the versioned contract each
    // published function answers to. It belongs in a SessionManifest for the
    // same reason the protocol schema hashes do -- a framework upgrade that
    // changed any of them would change what a plugin does under a session
    // manifest that had not moved, and what a function RETURNS is such a
    // change even when every published name is unmoved.
    [[nodiscard]]
    auto pluginEnvironmentHash() -> Result<ContentHash>;
} // namespace uf::script
