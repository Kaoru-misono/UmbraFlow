#pragma once

#include "catalog.hpp"
#include "content-hash.hpp"

#include <core/error/result.hpp>
#include <core/safety/annotations.hpp>
#include <core/types/integer.hpp>
#include <core/types/strong-value.hpp>

#include <domain/ids.hpp>

#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace uf::annotation
{
    inline constexpr auto k_authoringDocumentSchema = std::string_view{
        "umbraflow-authoring/v1"
    };

    namespace detail
    {
        struct SourceIdTag;
        struct RegressionIdTag;
    }

    using SourceId = StrongValue<detail::SourceIdTag, ResourceId>;
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

    // An authored recognizer: the definition the runtime will see, plus the two
    // facts only authoring keeps. The screen it was drawn on is what makes its
    // rectangles meaningful; the shared flag records that the author intends
    // these pixels to be reused on other pages, which is a statement of intent
    // no other field can carry -- an element marked shared before it reaches a
    // second page is indistinguishable from an ordinary one without it.
    //
    // Neither reaches the runtime manifest: recognition needs the template and
    // the region, never where the author got them.
    struct AuthoringRecognizerSpec final
    {
        RecognizerDefinition definition;
        SourceId             sourceId;
        bool                 shared{};
    };

    struct AuthoringRecognizerSource final
    {
        RecognizerId recognizerId;
        SourceId     sourceId;
        bool         shared{};
    };

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
        RecognitionCatalog                     m_catalog;
        std::vector<AuthoringSource>           m_sources;
        std::vector<AuthoringRecognizerSource> m_recognizerSources;
        std::vector<RegressionCase>            m_regressions;

        // LLVM 23's performance-unnecessary-value-param check can recurse
        // through StrongValue construction when these owned sinks are passed
        // by value.
        AuthoringDocument(
            RecognitionCatalog&& catalog,
            std::vector<AuthoringSource>&& sources,
            std::vector<AuthoringRecognizerSource>&& recognizerSources,
            std::vector<RegressionCase>&& regressions
        ) noexcept;

    public:
        [[nodiscard]]
        static auto create(
            ProjectId projectId,
            ProjectFingerprint fingerprint,
            std::vector<AuthoringSource> sources,
            std::vector<AuthoringRecognizerSpec> recognizers,
            std::vector<PageSignature> pages,
            std::vector<RegressionCase> regressions
        ) -> Result<AuthoringDocument>;

        [[nodiscard]]
        auto catalog() const noexcept UF_LIFETIME_BOUND -> RecognitionCatalog const&;

        [[nodiscard]]
        auto sources() const noexcept UF_LIFETIME_BOUND -> std::span<AuthoringSource const>;

        [[nodiscard]]
        auto recognizerSources() const noexcept UF_LIFETIME_BOUND
            -> std::span<AuthoringRecognizerSource const>;

        [[nodiscard]]
        auto regressions() const noexcept UF_LIFETIME_BOUND -> std::span<RegressionCase const>;

        [[nodiscard]]
        auto findSource(
            SourceId id
        ) const noexcept UF_LIFETIME_BOUND -> AuthoringSource const*;
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
