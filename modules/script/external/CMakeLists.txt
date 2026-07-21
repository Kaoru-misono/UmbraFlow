# Luau (pinned submodule external/luau @ 0.730) is the task-scripting engine for
# the script module. It is confined to this module: the AutoLoader picks up this
# external/ directory during the module define pass, so nothing leaks to the
# top-level build. The module links only Luau.VM + Luau.Compiler (Ast / Bytecode
# / Common arrive transitively).
set(LUAU_BUILD_CLI   OFF CACHE BOOL "" FORCE)
set(LUAU_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LUAU_BUILD_WEB   OFF CACHE BOOL "" FORCE)

add_subdirectory(luau EXCLUDE_FROM_ALL)
