#pragma once

#include "capabilities.hpp"
#include "catalog.hpp"
#include "content-hash.hpp"
#include "resource.hpp"
#include "appearance.hpp"

#include <core/error/result.hpp>
#include <core/safety/annotations.hpp>
#include <core/types/integer.hpp>
#include <core/types/strong-value.hpp>

#include <domain/ids.hpp>

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace uf::annotation
{
    // The schema the authoring tools read and write. Bumped from v3 on
    // 2026-07-31 by the vocabulary rename: `[[annotation]]` is now
    // `[[element]]`, `[[variant]]` is now `[[appearance]]`, a reference's
    // `variant` field is now `appearance`, and `recognizer_kind` is now
    // `element_kind`. `[[annotation]]` rode in with the three-way
    // anchor/target/info taxonomy the capability model retired, so it named a
    // classification that no longer exists; it moves in this bump rather than
    // costing a v5 of its own. v3 was itself the bump for the capability set,
    // page-side holding, and the derived page signature. Every earlier read path
    // is retired the same way: an old schema string fails with the ordinary
    // unsupported-schema error rather than upgrading.
    inline constexpr auto k_authoringDocumentSchema = std::string_view{
        "umbraflow-authoring/v4"
    };

    namespace detail
    {
        struct RegressionIdTag;
    }

    using RegressionId = StrongValue<detail::RegressionIdTag, ResourceId>;

    struct WgcSourceProvenance final
    {
        TargetGeneration targetGeneration{};
        std::string      capturedAt{};

        auto operator==(WgcSourceProvenance const&) const -> bool = default;
    };

    struct ImportedSourceProvenance final
    {
        auto operator==(ImportedSourceProvenance const&) const -> bool = default;
    };

    using SourceProvenance = std::variant<
        WgcSourceProvenance,
        ImportedSourceProvenance
    >;

    struct AuthoringSourceSpec final
    {
        SourceId           id;
        ContentHash        contentHash;
        ProjectFingerprint fingerprint;
        SourceProvenance   provenance{};
    };

    class AuthoringSource final
    {
        SourceId           m_id;
        ContentHash        m_contentHash;
        std::string        m_relativePath;
        ProjectFingerprint m_fingerprint;
        SourceProvenance   m_provenance;

        explicit AuthoringSource(AuthoringSourceSpec const& spec);

    public:
        [[nodiscard]]
        static auto create(
            AuthoringSourceSpec const& spec
        ) -> Result<AuthoringSource>;

        [[nodiscard]] auto id() const -> SourceId;
        [[nodiscard]] auto contentHash() const -> ContentHash;
        [[nodiscard]] auto fingerprint() const noexcept -> ProjectFingerprint;

        [[nodiscard]]
        auto relativePath() const noexcept UF_LIFETIME_BOUND -> std::string const&;

        [[nodiscard]]
        auto provenance() const noexcept UF_LIFETIME_BOUND -> SourceProvenance const&;
    };

    // An authored element: one rectangle of the screen, what it may be used
    // for, and the appearances it can take. Where it is used is a page-side
    // fact and lives on PageReference; nothing here says which page.
    //
    // An empty appearance list is a legal and meaningful state: it says this
    // rectangle is located by the page being recognised rather than by pixels
    // of its own, which is what a readable cell and a click target inside an
    // already-identified page both need. Its cost is the one section 2.1
    // accepted for reading -- if the layout moves, the rectangle points at the
    // wrong place -- and its consequence is that such an element can never be
    // identity evidence.
    class Element final
    {
    public:
        struct Spec final
        {
            ElementId           id;
            ResourceName        name;
            ElementCapabilities capabilities;
            PixelRect           searchRoi;

            // Ordered. Declaration order decides ties and nothing else: which
            // appearance matched is decided by normalized margin, because a wide
            // early appearance winning by arriving first would move the click.
            std::vector<Appearance> appearances{};
        };

    private:
        ElementId           m_id;
        ResourceName        m_name;
        ElementCapabilities m_capabilities;
        PixelRect           m_searchRoi;

        std::vector<Appearance> m_appearances;

        explicit Element(Spec spec) noexcept;

    public:
        [[nodiscard]]
        static auto create(
            ProjectFingerprint fingerprint,
            Spec const& spec
        ) -> Result<Element>;

        [[nodiscard]] auto id() const -> ElementId;
        [[nodiscard]] auto name() const -> ResourceName;

        [[nodiscard]]
        auto capabilities() const noexcept UF_LIFETIME_BOUND -> ElementCapabilities const&;

        [[nodiscard]] auto searchRoi() const noexcept -> PixelRect;

        [[nodiscard]]
        auto appearances() const noexcept UF_LIFETIME_BOUND -> std::span<Appearance const>;

        // Returned observations remain valid only while this element lives.
        [[nodiscard]]
        auto findAppearance(
            ResourceName const& name
        ) const noexcept UF_LIFETIME_BOUND -> Appearance const*;
    };

    // The runtime's view of one authored appearance. The source it was cut from
    // and the colour key that masked it are authoring truth and stop here: the
    // runtime reads the mask off the compiled template's alpha channel.
    [[nodiscard]]
    auto runtimeAppearanceOf(Appearance const& appearance) -> CompiledAppearance;

    enum class RegressionClassification : uint8
    {
        Positive,
        Negative,
        Confusable,
    };

    struct ResolvedRegression final
    {
        PageId pageId;

        auto operator==(ResolvedRegression const&) const -> bool = default;
    };

    struct UnknownRegression final
    {
        auto operator==(UnknownRegression const&) const -> bool = default;
    };

    struct AmbiguousRegression final
    {
        auto operator==(AmbiguousRegression const&) const -> bool = default;
    };

    using RegressionExpectation = std::variant<
        ResolvedRegression,
        UnknownRegression,
        AmbiguousRegression
    >;

    struct RegressionSpec final
    {
        RegressionId             id;
        SourceId                 sourceId;
        RegressionClassification classification{};
        RegressionExpectation    expectation;
    };

    class RegressionCase final
    {
        RegressionId             m_id;
        SourceId                 m_sourceId;
        RegressionClassification m_classification;
        RegressionExpectation    m_expectation;

    public:
        explicit RegressionCase(RegressionSpec const& spec);

        [[nodiscard]] auto id() const -> RegressionId;
        [[nodiscard]] auto sourceId() const -> SourceId;

        [[nodiscard]]
        auto classification() const noexcept -> RegressionClassification;

        [[nodiscard]]
        auto expectation() const noexcept UF_LIFETIME_BOUND -> RegressionExpectation const&;
    };

    class AuthoringDocument final
    {
        // m_elements is the authoring truth; m_catalog is the read model
        // derived from it at construction, and it is also where the page
        // references live, because the reference is the same value on both
        // sides. Every model invariant that spans elements and pages is
        // enforced by RecognitionCatalog::create, so the authoring document and
        // the runtime manifest cannot disagree about what is well formed.
        RecognitionCatalog           m_catalog;
        std::vector<AuthoringSource> m_sources;
        std::vector<Element>         m_elements;
        std::vector<RegressionCase>  m_regressions;

        // LLVM 23's performance-unnecessary-value-param check can recurse
        // through StrongValue construction when these owned sinks are passed
        // by value.
        AuthoringDocument(
            RecognitionCatalog&& catalog,
            std::vector<AuthoringSource>&& sources,
            std::vector<Element>&& elements,
            std::vector<RegressionCase>&& regressions
        ) noexcept;

    public:
        [[nodiscard]]
        static auto create(
            ProjectId projectId,
            ProjectFingerprint fingerprint,
            std::vector<AuthoringSource> sources,
            std::vector<Element> elements,
            std::vector<PageSpec> pages,
            std::vector<PageReference> references,
            std::vector<RegressionCase> regressions
        ) -> Result<AuthoringDocument>;

        [[nodiscard]]
        auto catalog() const noexcept UF_LIFETIME_BOUND -> RecognitionCatalog const&;

        [[nodiscard]]
        auto sources() const noexcept UF_LIFETIME_BOUND -> std::span<AuthoringSource const>;

        [[nodiscard]]
        auto elements() const noexcept UF_LIFETIME_BOUND -> std::span<Element const>;

        [[nodiscard]]
        auto references() const noexcept UF_LIFETIME_BOUND -> std::span<PageReference const>;

        [[nodiscard]]
        auto regressions() const noexcept UF_LIFETIME_BOUND -> std::span<RegressionCase const>;

        [[nodiscard]]
        auto findSource(
            SourceId id
        ) const noexcept UF_LIFETIME_BOUND -> AuthoringSource const*;

        [[nodiscard]]
        auto findElement(
            ElementId id
        ) const noexcept UF_LIFETIME_BOUND -> Element const*;
    };

    [[nodiscard]]
    auto serializeAuthoringDocument(
        AuthoringDocument const& document
    ) -> std::string;

    [[nodiscard]]
    auto parseAuthoringDocument(
        std::string_view canonicalToml
    ) -> Result<AuthoringDocument>;
}
