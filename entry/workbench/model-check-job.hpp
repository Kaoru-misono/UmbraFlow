#pragma once

#include "preview.hpp"

#include <annotation/authoring-compiler.hpp>
#include <annotation/authoring-document.hpp>
#include <annotation/recognition-runtime.hpp>

#include <core/error/result.hpp>

#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <thread>
#include <vector>

namespace uf::workbench
{
    // What one background run computes, given the token that cancels it.
    using ModelCheckWork = std::function<Result<ModelCheck>(std::stop_token)>;

    // Runs runModelCheck off the GUI thread and hands the result back one frame
    // at a time.
    //
    // The search is not cheap and does not get cheaper: a 207x368 template swept
    // through a 260x420 region is over two hundred million pixel comparisons for
    // one recognizer against one screen, and the check runs every recognizer
    // against every screen. On the GUI thread that is seconds of a frozen window.
    //
    // The worker owns copies of everything it reads. It cannot borrow the
    // document, because the author keeps editing it while the check runs, and a
    // committed edit replaces the document wholesale. Copying the source PNGs
    // costs a few megabytes per run, which is the price of the worker never
    // touching state the GUI thread owns.
    class ModelCheckJob final
    {
        // The one object both threads touch, held by shared_ptr so the worker
        // keeps it alive even if the job is destroyed mid-run. Every field is
        // guarded by m_mutex.
        struct Slot final
        {
            std::mutex                        m_mutex{};
            std::optional<Result<ModelCheck>> m_result{};
            bool                              m_finished{};
        };

        std::shared_ptr<Slot> m_slot{};
        std::jthread          m_worker{};

    public:
        ModelCheckJob() = default;

        ModelCheckJob(ModelCheckJob const&)                    = delete;
        auto operator=(ModelCheckJob const&) -> ModelCheckJob& = delete;
        ModelCheckJob(ModelCheckJob&&)                         = delete;
        auto operator=(ModelCheckJob&&) -> ModelCheckJob&      = delete;

        // Requests cancellation and joins, so no worker outlives the job. The
        // policy's stop token is the one the worker was started with, and the
        // recognition runtime checks it between comparisons.
        ~ModelCheckJob();

        // Starts a check over copies of the document, the assets, and the live
        // frame. Does nothing while one is already running: a second check would
        // answer a question the first is already answering, and the caller
        // disables the button.
        //
        // liveFrameBytes is empty when no frame was captured. Capture stays with
        // the caller because it belongs to the GUI thread that owns the graphics
        // device; only the searching moves off it.
        //
        // This is where a discarded worker is finally joined, so it can block
        // the caller -- and "edit something, then check again" is the common way
        // to reach it. The wait is short because the recognition runtime polls
        // the stop token inside the pixel sweep rather than between screens.
        auto start(
            annotation::AuthoringDocument const& document,
            std::span<annotation::AuthoringSourceAsset const> sourceAssets,
            std::span<std::byte const> liveFrameBytes,
            annotation::RecognitionPolicy const& policy
        ) -> void;

        // Runs one unit of work on the job's thread and delivers what it returns.
        // start() is this plus the copies a real check needs, and everything that
        // can go wrong with a background job -- delivering twice, delivering after
        // a discard, outliving the job, refusing a second run -- lives here rather
        // than in the search. Tests drive this directly, because none of those
        // properties need two hundred million pixel comparisons to observe.
        //
        // The work is owned by value and moved onto the worker, so it must capture
        // copies: a lambda that borrows the caller's state would outlive it. Its
        // stop_token is the worker's own, and honouring it is what keeps the wait
        // in start() and the destructor short.
        auto startWith(ModelCheckWork work) -> void;

        [[nodiscard]] auto running() const -> bool;

        // Abandons a running check because its answer would describe a document
        // that no longer exists. Requests cancellation and drops the shared slot,
        // so the worker's write lands in storage nobody reads and takeResult
        // never yields it. Does not block here; the thread is joined by the next
        // start or by the destructor, so the wait lands on whichever comes first.
        auto discard() -> void;

        // The finished result, moved out so it is delivered exactly once, or
        // nothing while the worker is still going. Joins the finished worker
        // before returning, so a job that has yielded its result holds no thread.
        [[nodiscard]] auto takeResult() -> std::optional<Result<ModelCheck>>;
    };
}
