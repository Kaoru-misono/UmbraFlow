#pragma once

#include <core/error/result.hpp>

#include <script/engine.hpp>

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace uf::task
{
    // One module of the trusted Luau framework, embedded into the binary at build
    // time by scripts/embed_luau.py from modules/task/runtime.
    //
    // Lifetime contract: every view below borrows a string literal with static
    // storage duration inside the generated translation unit, so an entry and its
    // views stay valid for the whole process and may be stored without further
    // arrangement. This is the deliberate exception to the project's "do not store
    // views" rule -- the backing owner is the program image itself.
    struct FrameworkBundleEntry final
    {
        // The Luau module name: the .luau file stem, never a path.
        std::string_view name{};

        // The module's UTF-8 source text, byte for byte as it is on disk.
        std::string_view source{};

        // Lowercase hex SHA-256 of `source`, computed by the generator at build
        // time. tests/task/test-framework-bundle.cpp recomputes it with sha256,
        // which keeps the build-time and run-time hash definitions from drifting.
        std::string_view sourceHash{};
    };

    // The framework bundle, ordered by the generator's sort over the source
    // paths, so the sequence is identical on every machine and every build.
    [[nodiscard]]
    auto frameworkBundleEntries() noexcept -> std::span<FrameworkBundleEntry const>;

    // Lowercase hex SHA-256 over the whole bundle. Recipe: concatenate, for each
    // entry of frameworkBundleEntries() in order, the module name in UTF-8, one
    // 0x00 separator byte, then that module's source bytes, and take the SHA-256
    // of that byte string. NUL occurs in neither a module name nor a source, so
    // the concatenation is unambiguous.
    //
    // It stamps a trace so one run is attributable to an exact framework build,
    // and catches a bundle that went accidentally stale. It is NOT a security
    // property and must not be described as one: the digest is compiled into the
    // same binary as the bytes it certifies.
    [[nodiscard]]
    auto frameworkBundleHash() noexcept -> std::string_view;

    // The framework's semantic version, declared as [embed].luau_version in
    // modules/task/manifest.txt and stamped into the bundle at build time.
    [[nodiscard]]
    auto frameworkVersion() noexcept -> std::string_view;

    // The same bundle, in the shape script::EngineConfig::frameworkModules takes,
    // so every VM that boots this framework builds the list one way. The views it
    // carries are the bundle's own, so they live for the whole process and the
    // returned vector may be handed to a config that outlives this call. Only the
    // vector itself is fresh, and this runs once per VM.
    [[nodiscard]]
    auto frameworkScriptModules() -> std::vector<script::FrameworkModule>;

    // Business execution is closed until OperatorSession exists, so this list is
    // deliberately empty. Loading trusted modules into a VM does not make their
    // exports project-visible; publication remains an explicit whitelist.
    [[nodiscard]]
    auto frameworkProjectGlobals() -> std::vector<std::string>;

    // The privileged authoring VM publishes only `explore`. The module owns its
    // cycle bracket and confined project I/O; no raw native table or direct input
    // primitive enters the project environment.
    [[nodiscard]]
    auto explorationProjectGlobals() -> std::vector<std::string>;

    // Parses one framework module with the same vendored Luau parser the host uses
    // at load time, and reports the first syntax error together with its line and
    // column. `chunkName` only labels the diagnostic.
    //
    // This is the repository's syntax gate for .luau sources: the bundle test runs
    // it over every embedded entry, so a malformed framework module fails under
    // `ctest -L CI` rather than at the VM. A failure is InternalInvariant rather
    // than InvalidResource, because the framework is first-party and compiled into
    // the binary -- a syntax error there is a broken host, not bad user input.
    //
    // No Luau type appears in this header; the parse lives behind the module's ffi
    // boundary.
    [[nodiscard]]
    auto checkFrameworkModuleSyntax(
        std::string_view source,
        std::string_view chunkName
    ) -> Status;
}
