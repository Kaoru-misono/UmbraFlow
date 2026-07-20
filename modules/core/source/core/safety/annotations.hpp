#pragma once

#if defined(__clang__)
    #define UF_LIFETIME_BOUND [[clang::lifetimebound]]
    #define UF_NO_ESCAPE [[clang::noescape]]
    #define UF_UNSAFE_BUFFER_USAGE [[clang::unsafe_buffer_usage]]

    #define UF_CAPABILITY(name) [[clang::capability(name)]]
    #define UF_SCOPED_CAPABILITY [[clang::scoped_lockable]]
    #define UF_GUARDED_BY(capability) [[clang::guarded_by(capability)]]
    #define UF_REQUIRES_CAPABILITY(...) [[clang::requires_capability(__VA_ARGS__)]]
    #define UF_ACQUIRE_CAPABILITY(...) [[clang::acquire_capability(__VA_ARGS__)]]
    #define UF_RELEASE_CAPABILITY(...) [[clang::release_capability(__VA_ARGS__)]]
    #define UF_EXCLUDES_CAPABILITY(...) [[clang::locks_excluded(__VA_ARGS__)]]
    #define UF_NO_THREAD_SAFETY_ANALYSIS [[clang::no_thread_safety_analysis]]
#else
    #define UF_LIFETIME_BOUND
    #define UF_NO_ESCAPE
    #define UF_UNSAFE_BUFFER_USAGE

    #define UF_CAPABILITY(name)
    #define UF_SCOPED_CAPABILITY
    #define UF_GUARDED_BY(capability)
    #define UF_REQUIRES_CAPABILITY(...)
    #define UF_ACQUIRE_CAPABILITY(...)
    #define UF_RELEASE_CAPABILITY(...)
    #define UF_EXCLUDES_CAPABILITY(...)
    #define UF_NO_THREAD_SAFETY_ANALYSIS
#endif
