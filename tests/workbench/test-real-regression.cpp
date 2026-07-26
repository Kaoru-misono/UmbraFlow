#include <project-persistence.hpp>

#include <annotation/authoring-document.hpp>
#include <annotation/recognition-runtime.hpp>
#include <annotation/regression-runner.hpp>

#include <core/error/error.hpp>
#include <core/time/monotonic-time.hpp>
#include <core/types/integer.hpp>

#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#ifndef UF_REAL_REGRESSION_ROOT
#error "UF_REAL_REGRESSION_ROOT must be defined to build the real-regression test"
#endif

// Local-only harness for real game screenshots that are never committed. The
// asset directory is git-ignored and absent on CI, so the CMake target is
// registered only when it exists on a developer machine. The suite walks every
// direct subdirectory as an independent authoring project, reruns its recorded
// regression cases, and holds each one to its stored classification.
namespace uf::workbench
{
    namespace
    {
        // A generous ceiling that never bounds a real project's comparisons.
        constexpr auto k_comparisonBudget = uint64{1} << 30;

        // Per-project wall-clock safety bound shared across one suite run.
        constexpr auto k_deadlineWindow = std::chrono::seconds{30};

        [[nodiscard]]
        auto projectDirectories(
            std::filesystem::path const& root
        ) -> std::vector<std::filesystem::path>
        {
            auto error    = std::error_code{};
            auto iterator = std::filesystem::directory_iterator{root, error};
            REQUIRE_FALSE(error);

            auto directories = std::vector<std::filesystem::path>{};
            auto const end   = std::filesystem::directory_iterator{};
            while (iterator != end)
            {
                auto const isDirectory = iterator->is_directory(error);
                REQUIRE_FALSE(error);
                if (isDirectory)
                {
                    directories.emplace_back(iterator->path());
                }
                iterator.increment(error);
                REQUIRE_FALSE(error);
            }
            return directories;
        }
    }

    TEST_CASE("real-screenshot authoring regressions match their recorded expectations")
    {
        auto const root = std::filesystem::path{UF_REAL_REGRESSION_ROOT};
        auto const directories = projectDirectories(root);
        if (directories.empty())
        {
            MESSAGE(
                "no real-regression project directories under "
                << root.string()
                << "; nothing to verify"
            );
            return;
        }

        for (auto const& directory : directories)
        {
            INFO("project: " << directory.filename().string());

            auto const loaded     = loadAuthoringProject(directory);
            auto const loadedInfo = loaded
                ? std::string{"loaded"}
                : toString(loaded.error());
            INFO("load: " << loadedInfo);
            REQUIRE(loaded.has_value());

            auto const deadline = MonotonicInstant::now().checkedAdd(
                k_deadlineWindow
            );
            REQUIRE(deadline.has_value());

            auto const report = annotation::runAuthoringRegressions(
                loaded->document,
                loaded->sources,
                annotation::RecognitionPolicy{
                    .maximumPixelComparisons = k_comparisonBudget,
                    .deadline                = deadline,
                }
            );
            auto const reportInfo = report
                ? std::string{"ran"}
                : toString(report.error());
            INFO("run: " << reportInfo);
            REQUIRE(report.has_value());
            REQUIRE(report->completedAllCases);

            for (auto const& caseReport : report->cases)
            {
                INFO(
                    "case "
                    << caseReport.id.value().toString()
                    << " (source "
                    << caseReport.sourceId.value().toString()
                    << ")"
                );
                CHECK(caseReport.matchesExpectation);
            }
        }
    }
}
