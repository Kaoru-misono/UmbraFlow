#pragma once

#include <core/error/result.hpp>

#include <span>
#include <string_view>

namespace uf::task
{
    // One module of the trusted Luau framework, embedded into the binary at
    // build time by scripts/embed_luau.py from modules/task/runtime.
    //
    // Lifetime contract: every view below borrows a string literal with static
    // storage duration inside the generated translation unit. An entry and the
    // views it carries therefore stay valid for the whole process and may be
    // stored without any further arrangement. This is the deliberate exception
    // to the project's "do not store views" rule -- the backing owner is the
    // program image itself, which outlives every possible observer.
    struct FrameworkBundleEntry final
    {
        // The Luau module name: the .luau file stem, never a path.
        std::string_view name{};

        // The module's UTF-8 source text, byte for byte as it is on disk.
        std::string_view source{};

        // Lowercase hex SHA-256 of `source`, computed by the generator at build
        // time. tests/task/test-framework-bundle.cpp recomputes it with
        // annotation::sha256, which is what keeps the build-time and run-time
        // hash definitions from drifting apart.
        std::string_view sourceHash{};
    };

    // The framework bundle, ordered by the generator's sort over the source
    // paths, so the sequence is identical on every machine and every build.
    [[nodiscard]]
    auto frameworkBundleEntries() noexcept -> std::span<FrameworkBundleEntry const>;

    // Lowercase hex SHA-256 over the whole bundle.
    //
    // Recipe, recomputable by hand: concatenate, for each entry of
    // frameworkBundleEntries() in order, the module name in UTF-8, one 0x00
    // separator byte, then that module's source bytes; the bundle hash is the
    // SHA-256 of that byte string. NUL occurs in neither a module name nor a
    // source, so the concatenation is unambiguous.
    //
    // What this hash is for, stated plainly: stamping a trace so one run is
    // attributable to an exact framework build, and catching a bundle that went
    // accidentally stale. It is NOT a security property and must not be
    // described as one. The digest is compiled into the same binary as the bytes
    // it certifies, so it proves nothing against anyone able to rewrite that
    // binary -- they would simply rewrite the digest too.
    [[nodiscard]]
    auto frameworkBundleHash() noexcept -> std::string_view;

    // The framework's semantic version, declared as [embed].luau_version in
    // modules/task/manifest.txt and stamped into the bundle at build time.
    [[nodiscard]]
    auto frameworkVersion() noexcept -> std::string_view;

    // Parses one framework module with the same vendored Luau parser the host
    // uses at load time, and reports the first syntax error together with its
    // line and column. `chunkName` only labels the diagnostic.
    //
    // This is the repository's syntax gate for .luau sources: the bundle test
    // runs it over every embedded entry, so a malformed framework module fails
    // under `ctest -L CI` at the same moment it would otherwise have reached the
    // VM. A failure is InternalInvariant rather than InvalidResource, because
    // the framework is first-party and compiled into the binary -- a syntax
    // error there is a broken host, not bad user input.
    //
    // No Luau type appears in this header. The parse lives behind the module's
    // ffi boundary, exactly as validateScriptResources does.
    [[nodiscard]]
    auto checkFrameworkModuleSyntax(
        std::string_view source,
        std::string_view chunkName
    ) -> Status;
}
