// The compile-time half of `caller independence is structural`.
//
// That ruling's claim is not that adapters are careful. It is that an adapter
// is definitionally a translator: it can build one ToolAdmissionRequest and
// nothing else it is able to construct is executable, so the failure mode of a
// new caller changes kind -- from "it evaluated authority differently and no
// fixture covered that case" to "it does not compile".
//
// A claim of that shape cannot be witnessed by a passing test, because what it
// forbids is code that does not exist. It is witnessed by refusing to compile,
// which is what this file states. Most of it is therefore negative; the
// positives pin what a translator IS allowed to do, so that the negatives read
// as "an adapter builds a request" rather than as "an adapter can do nothing",
// and one of them records a deliberate decision to leave a value copyable.
//
// The runtime half -- that the refusals inside admission are reachable and
// exercised -- lives in test-ledger.cpp, test-tool-nested-calls.cpp and
// test-tool-executor.cpp, which drive the same one funnel. The semantic half --
// that four producers translate the same input the same way -- is the four-way
// fixture, and it lands with the second producer.

#include <operator/ledger.hpp>
#include <operator/tool-admission-request.hpp>
#include <operator/tool-executor.hpp>
#include <operator/tool-invocation.hpp>
#include <operator/tool-runtime.hpp>

#include <domain/content-hash.hpp>

#include <core/types/integer.hpp>

#include <concepts>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

namespace uf::operator_runtime
{
    namespace
    {
        // Nothing downstream of admission is constructible outside the runtime.
        // Each of these names the exact private signature the Coordinator mints
        // through, so a constructor that stopped being private would satisfy
        // the concept and fail the assertion rather than quietly widening the
        // seam.
        static_assert(
            !std::constructible_from<ToolCallAdmission, ContentHash, uint64, uint64>,
            "An adapter must not be able to construct an admitted call"
        );
        static_assert(
            !std::constructible_from<ToolCallDispatch, ContentHash, uint64, uint64>,
            "An adapter must not be able to construct the capability to execute"
        );
        static_assert(
            !std::constructible_from<
                ToolDelegationGrant,
                std::string,
                ContentHash,
                ContentHash,
                uint64,
                std::string
            >,
            "An adapter must not be able to construct a delegation grant"
        );

        // The coordinate is the oldest member of this family and the pattern
        // the ruling says it is extending: the position factory is private to
        // the issuing context, so there is no door a producer can hand an
        // ordinal, a parent, or a run identity to.
        template <typename... Parts>
        concept CoordinateMintable = requires(Parts const&... parts) {
            ToolCallPositionIdentity::create(parts...);
        };

        static_assert(
            !CoordinateMintable<
                ContentHash,
                ToolCallParent,
                uint32,
                ToolExecutionIdentity,
                ValidatedToolInvocation,
                std::optional<ContentHash>
            >,
            "Only the issuing context mints a call coordinate"
        );

        // Move-only is the ruling's word for the capability to execute, and the
        // admitted call it is obtained from carries the same discipline. What
        // is forbidden is a second independent holder, never a single holder
        // retrying, so the assertions are about copying and not about use.
        static_assert(
            std::move_constructible<ToolCallAdmission>
                && !std::copy_constructible<ToolCallAdmission>
                && !std::is_copy_assignable_v<ToolCallAdmission>,
            "An admitted call is a capability one holder moves, never duplicates"
        );
        static_assert(
            std::move_constructible<ToolCallDispatch>
                && !std::copy_constructible<ToolCallDispatch>
                && !std::is_copy_assignable_v<ToolCallDispatch>,
            "The capability to execute is a capability one holder moves, never "
            "duplicates"
        );

        // Deliberately positive. A delegation grant is evidence re-verified
        // against the live parent row at every use and re-derivable by anyone
        // who can already name the parent, so a duplicate confers nothing and
        // expires exactly when the original does. Pinning that here means a
        // later change of mind has to be a change to this line and its reason
        // rather than a silent drift into a third category.
        static_assert(
            std::copy_constructible<ToolDelegationGrant>,
            "A delegation grant is evidence, not a licence to run anything"
        );

        // One funnel. The nine-parameter spelling admitToolCall carried before
        // this ruling, and the four identity values an adapter would most
        // plausibly reach for, are both gone: there is no overload set to pick
        // from and no second door beside it.
        template <typename... Parts>
        concept AdmissionAccepts = requires(
            OperatorCoordinator& coordinator,
            Parts const&... parts
        ) { coordinator.admitToolCall(parts...); };

        static_assert(
            AdmissionAccepts<ToolAdmissionRequest>,
            "Admission accepts the one request value"
        );
        static_assert(
            !AdmissionAccepts<
                ControllerBinding,
                ControlLease,
                ToolRootRequestIdentity,
                ToolCallPositionIdentity
            >,
            "Admission must not accept the request's members loose"
        );
        static_assert(
            !AdmissionAccepts<
                ControllerBinding,
                ControlLease,
                ToolRootRequestIdentity,
                ToolCallPositionIdentity,
                ToolMutability,
                OperatorPolicyAuthority,
                std::vector<ProposedEffect>,
                std::vector<ToolApprovalGrant>,
                ToolDelegationGrant
            >,
            "The pre-ruling nine-parameter admission must not exist"
        );

        // The same statement one layer up, because the executor is what a
        // producer actually calls and a loose-parts seam there would put the
        // funnel back where it was.
        template <typename... Parts>
        concept InvokeAccepts = requires(
            ToolRuntimeExecutor& executor,
            Parts const&... parts,
            ToolProvider const& provider
        ) { executor.invoke(parts..., provider); };

        static_assert(
            InvokeAccepts<ToolAdmissionRequest>,
            "The execution seam accepts the one request value"
        );
        static_assert(
            !InvokeAccepts<
                ControllerBinding,
                ControlLease,
                ToolRootRequestIdentity,
                ToolCallPositionIdentity
            >,
            "The execution seam must not accept the request's members loose"
        );

        // What a translator IS allowed to do, so that the negatives above are
        // read as "an adapter builds a request" rather than as "an adapter can
        // do nothing". The two members it cannot invent are already in its
        // hands here: the coordinate came from an issuing context and the grant
        // from the Coordinator.
        template <typename... Parts>
        concept RequestBuildableFrom = requires(Parts const&... parts) {
            ToolAdmissionRequest{parts...};
        };

        static_assert(
            RequestBuildableFrom<
                ControllerBinding,
                ControlLease,
                ToolRootRequestIdentity,
                ToolCallPositionIdentity,
                OperatorPolicyAuthority
            >,
            "A producer must be able to build the request it translates into"
        );

        // And the policy is not optional in that value. A request without one
        // is a call nothing could judge the surface of, so the aggregate must
        // refuse to be built without it rather than admitting on a default.
        static_assert(
            !RequestBuildableFrom<
                ControllerBinding,
                ControlLease,
                ToolRootRequestIdentity,
                ToolCallPositionIdentity
            >,
            "A request carries the policy that judges it"
        );
    }
}
