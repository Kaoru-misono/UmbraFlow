# Logging and Assertions

- `UMBRA_FLOW_ASSERT` reports debug-only programmer errors and invariants.
- `UMBRA_FLOW_CHECK` reports mandatory invariants in every build configuration.
- `UMBRA_FLOW_UNREACHABLE` marks impossible flow.
- Recoverable runtime failures travel through `Result<T>` or `Status`.
- `fail(...)` only constructs an error and never logs it.
- Log a propagated error once at the application or subsystem boundary.
- A local skip, clamp, or nullable degrade path must log its reason at the site because no structured error travels upward.
- Avoid `printf`, `std::cout`, and ad hoc logging inside framework modules.
- The contracts backend is the sole exception: it writes directly to standard
  error before terminating so it remains independent from the logging subsystem.
