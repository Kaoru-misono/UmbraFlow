#pragma once

#include "authoring-compiler.hpp"
#include "recognition-runtime.hpp"

#include <core/error/result.hpp>

#include <span>
#include <vector>

namespace uf::annotation
{
    struct RegressionCaseReport final
    {
        RegressionId             m_id;
        SourceId                 m_sourceId;
        RegressionClassification m_classification{};
        RegressionExpectation    m_expectation;
        PageRecognitionAttempt   m_attempt;
        bool                     m_matchesExpectation{};
    };

    struct RegressionSuiteReport final
    {
        std::vector<RegressionCaseReport> m_cases{};
        bool                              m_completedAllCases{};
    };

    // Compilation, closure validation, runtime construction, and decoding one
    // source at a time are preparation work outside the comparison policy.
    // The comparison budget restarts for each case. Cancellation and the
    // absolute deadline span the suite: either stops later cases, while budget
    // exhaustion remains a diagnostic for only the current case.
    [[nodiscard]]
    auto runAuthoringRegressions(
        AuthoringDocument const& document,
        std::span<AuthoringSourceAsset const> sourceAssets,
        RecognitionPolicy const& policy
    ) -> Result<RegressionSuiteReport>;
}
