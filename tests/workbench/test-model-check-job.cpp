#include "../annotation/test-helpers.hpp"

#include <model-check-job.hpp>
#include <preview.hpp>

#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <domain/error.hpp>

#include <doctest/doctest.h>

#include <chrono>
#include <latch>
#include <optional>
#include <semaphore>
#include <stop_token>
#include <thread>
#include <utility>

namespace uf::workbench
{
    namespace
    {
        // The job's contract is about delivery, cancellation and lifetime, none
        // of which depend on what the work computes. These fakes stand in for
        // runModelCheck so every property below is observed without a pixel
        // sweep, and without the wall-clock waits a real search would need.

        // A recognizer id is a required domain value with no default, so the
        // marker rides on a fixed one; which recognizer it names is irrelevant
        // here, only that the result carries the marker back.
        constexpr auto k_markerRecognizerId =
            "00000000-0000-0000-0000-000000000501";

        [[nodiscard]]
        auto markedResult(uint64 marker) -> ModelCheck
        {
            auto check = ModelCheck{};
            check.margins.emplace_back(
                RecognizerMargin{
                    .recognizerId = annotation::test::elementId(
                        k_markerRecognizerId
                    ),
                    .maximumSad = marker,
                }
            );
            return check;
        }

        [[nodiscard]]
        auto immediateWork(uint64 marker) -> ModelCheckJob::Work
        {
            return [marker](std::stop_token) -> Result<ModelCheck>
            {
                return markedResult(marker);
            };
        }

        // Blocks until the test releases it, so "still running" is a state the
        // test controls rather than one it races.
        struct Gate final
        {
            std::binary_semaphore release{0};
            std::latch            entered{1};

            auto work(uint64 marker) -> ModelCheckJob::Work
            {
                return [this, marker](std::stop_token) -> Result<ModelCheck>
                {
                    entered.count_down();
                    release.acquire();
                    return markedResult(marker);
                };
            }
        };

        [[nodiscard]]
        auto markerOf(std::optional<Result<ModelCheck>> const& taken) -> std::optional<uint64>
        {
            if (!taken.has_value() || !*taken || (*taken)->margins.empty())
            {
                return std::nullopt;
            }
            return (*taken)->margins.front().maximumSad;
        }

        // Spins takeResult until the worker has delivered. Every work above
        // either returns at once or is released by the test first, so this
        // cannot hang on a correct implementation; it only absorbs scheduling.
        [[nodiscard]]
        auto awaitResult(ModelCheckJob& job) -> std::optional<Result<ModelCheck>>
        {
            for (auto attempt = 0; attempt < 10'000; attempt += 1)
            {
                if (auto taken = job.takeResult(); taken.has_value())
                {
                    return taken;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
            return std::nullopt;
        }
    }

    TEST_CASE("a fresh job has nothing to deliver and is not running")
    {
        auto job = ModelCheckJob{};

        CHECK_FALSE(job.running());
        CHECK_FALSE(job.takeResult().has_value());
    }

    TEST_CASE("a finished run is delivered exactly once")
    {
        auto job = ModelCheckJob{};
        job.startWith(immediateWork(4242U));

        auto const taken = awaitResult(job);
        REQUIRE(taken.has_value());
        CHECK(markerOf(taken) == std::optional<uint64>{4242U});

        // Moved out on the first take, so a second poll in the next frame must
        // not hand the same verdict to the UI again.
        CHECK_FALSE(job.takeResult().has_value());
        CHECK_FALSE(job.running());
    }

    TEST_CASE("a failing run delivers its error rather than swallowing it")
    {
        auto job = ModelCheckJob{};
        job.startWith(
            [](std::stop_token) -> Result<ModelCheck>
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "model check could not build a runtime"
                );
            }
        );

        auto const taken = awaitResult(job);
        REQUIRE(taken.has_value());
        CHECK_FALSE(taken->has_value());
    }

    TEST_CASE("a job is running until its work returns")
    {
        auto job  = ModelCheckJob{};
        auto gate = Gate{};
        job.startWith(gate.work(7U));

        gate.entered.wait();
        CHECK(job.running());
        CHECK_FALSE(job.takeResult().has_value());

        gate.release.release();
        CHECK(markerOf(awaitResult(job)) == std::optional<uint64>{7U});
    }

    TEST_CASE("starting while a run is in flight leaves the first run alone")
    {
        auto job  = ModelCheckJob{};
        auto gate = Gate{};
        job.startWith(gate.work(1U));
        gate.entered.wait();

        // The caller disables the button, but a second press must not replace a
        // check already answering the same question.
        job.startWith(immediateWork(2U));

        gate.release.release();
        CHECK(markerOf(awaitResult(job)) == std::optional<uint64>{1U});
    }

    TEST_CASE("a discarded run is never delivered")
    {
        auto job  = ModelCheckJob{};
        auto gate = Gate{};
        job.startWith(gate.work(9U));
        gate.entered.wait();

        // An edit landed: the answer would describe a document that no longer
        // exists, so it must not reach the status line even though the worker
        // goes on to compute it.
        job.discard();
        gate.release.release();

        CHECK_FALSE(job.running());
        CHECK_FALSE(job.takeResult().has_value());
    }

    TEST_CASE("a discarded run does not block the next one")
    {
        auto job  = ModelCheckJob{};
        auto gate = Gate{};
        job.startWith(
            [&gate](std::stop_token stop) -> Result<ModelCheck>
            {
                gate.entered.count_down();
                while (!stop.stop_requested())
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds{1});
                }
                return markedResult(0U);
            }
        );
        gate.entered.wait();

        job.discard();

        // start joins the discarded worker, which is why the work above has to
        // honour the token: a run that ignored it would hang this call.
        job.startWith(immediateWork(5U));
        CHECK(markerOf(awaitResult(job)) == std::optional<uint64>{5U});
    }

    TEST_CASE("destroying a job cancels and joins its worker")
    {
        auto observedStop = false;
        auto entered      = std::latch{1};
        {
            auto job = ModelCheckJob{};
            job.startWith(
                [&observedStop, &entered](std::stop_token stop) -> Result<ModelCheck>
                {
                    entered.count_down();
                    while (!stop.stop_requested())
                    {
                        std::this_thread::sleep_for(std::chrono::milliseconds{1});
                    }
                    observedStop = true;
                    return markedResult(0U);
                }
            );
            entered.wait();
        }

        // Reaching here at all is the assertion: the destructor requested the
        // stop and joined, so no worker outlives the job and nothing it writes
        // lands in freed storage.
        CHECK(observedStop);
    }

    TEST_CASE("an empty unit of work is refused rather than started")
    {
        auto job = ModelCheckJob{};
        job.startWith(ModelCheckJob::Work{});

        CHECK_FALSE(job.running());
        CHECK_FALSE(job.takeResult().has_value());
    }
}
