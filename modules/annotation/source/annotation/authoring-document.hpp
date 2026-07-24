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
        TargetGeneration m_targetGeneration{};
        std::string      m_capturedAt{};

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
        SourceId           m_id;
        ContentHash        m_contentHash;
        ProjectFingerprint m_fingerprint;
        SourceProvenance   m_provenance{};
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

    struct AuthoringRecognizerSpec final
    {
        RecognizerDefinition m_definition;
        SourceId             m_sourceId;
    };

    struct AuthoringRecognizerSource final
    {
        RecognizerId m_recognizerId;
        SourceId     m_sourceId;
    };

    enum class RegressionClassification : uint8
    {
        Positive,
        Negative,
        Confusable,
    };

    struct ResolvedRegression final
    {
        PageId m_pageId;

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
        RegressionId             m_id;
        SourceId                 m_sourceId;
        RegressionClassification m_classification{};
        RegressionExpectation    m_expectation;
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
