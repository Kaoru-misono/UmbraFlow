#pragma once

#include "authoring-compiler.hpp"
#include "recognition-runtime.hpp"
#include "resource.hpp"

#include <core/error/result.hpp>

#include <span>
#include <vector>

namespace uf::annotation
{
    struct RegressionCaseReport final
    {
        RegressionId             id;
        SourceId                 sourceId;
        RegressionClassification classification{};
        RegressionExpectation    expectation;
        PageRecognitionAttempt   attempt;
        bool                     matchesExpectation{};
    };

    struct RegressionSuiteReport final
    {
        std::vector<RegressionCaseReport> cases{};
        bool                              completedAllCases{};
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
