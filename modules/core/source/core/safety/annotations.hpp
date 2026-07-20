#pragma once

#if defined(__clang__)
    #define UMBRA_FLOW_LIFETIME_BOUND [[clang::lifetimebound]]
    #define UMBRA_FLOW_NO_ESCAPE [[clang::noescape]]
    #define UMBRA_FLOW_UNSAFE_BUFFER_USAGE [[clang::unsafe_buffer_usage]]

    #define UMBRA_FLOW_CAPABILITY(name) [[clang::capability(name)]]
    #define UMBRA_FLOW_SCOPED_CAPABILITY [[clang::scoped_lockable]]
    #define UMBRA_FLOW_GUARDED_BY(capability) [[clang::guarded_by(capability)]]
    #define UMBRA_FLOW_REQUIRES_CAPABILITY(...) [[clang::requires_capability(__VA_ARGS__)]]
    #define UMBRA_FLOW_ACQUIRE_CAPABILITY(...) [[clang::acquire_capability(__VA_ARGS__)]]
    #define UMBRA_FLOW_RELEASE_CAPABILITY(...) [[clang::release_capability(__VA_ARGS__)]]
    #define UMBRA_FLOW_EXCLUDES_CAPABILITY(...) [[clang::locks_excluded(__VA_ARGS__)]]
    #define UMBRA_FLOW_NO_THREAD_SAFETY_ANALYSIS [[clang::no_thread_safety_analysis]]
#else
    #define UMBRA_FLOW_LIFETIME_BOUND
    #define UMBRA_FLOW_NO_ESCAPE
    #define UMBRA_FLOW_UNSAFE_BUFFER_USAGE

    #define UMBRA_FLOW_CAPABILITY(name)
    #define UMBRA_FLOW_SCOPED_CAPABILITY
    #define UMBRA_FLOW_GUARDED_BY(capability)
    #define UMBRA_FLOW_REQUIRES_CAPABILITY(...)
    #define UMBRA_FLOW_ACQUIRE_CAPABILITY(...)
    #define UMBRA_FLOW_RELEASE_CAPABILITY(...)
    #define UMBRA_FLOW_EXCLUDES_CAPABILITY(...)
    #define UMBRA_FLOW_NO_THREAD_SAFETY_ANALYSIS
#endif
