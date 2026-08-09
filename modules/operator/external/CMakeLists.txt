# SQLite is confined to the Operator persistence adapter. Public Operator
# headers expose ordinary values and never expose sqlite3 handles.
enable_language(C)

set(UF_OPERATOR_SQLITE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/sqlite")
if(NOT EXISTS "${UF_OPERATOR_SQLITE_ROOT}/sqlite3.c")
    message(FATAL_ERROR
        "[External] operator: SQLite is missing from ${UF_OPERATOR_SQLITE_ROOT}.\n"
        "  Restore it with: python scripts/fetch_external.py --module operator"
    )
endif()

add_library(${PROJECT_NAME}_operator_sqlite3 STATIC
    "${UF_OPERATOR_SQLITE_ROOT}/sqlite3.c"
)

target_include_directories(${PROJECT_NAME}_operator_sqlite3 SYSTEM PUBLIC
    "${UF_OPERATOR_SQLITE_ROOT}"
)

target_compile_definitions(${PROJECT_NAME}_operator_sqlite3 PRIVATE
    SQLITE_DEFAULT_FOREIGN_KEYS=1
    SQLITE_DQS=0
    SQLITE_OMIT_DEPRECATED=1
    SQLITE_OMIT_LOAD_EXTENSION=1
    SQLITE_THREADSAFE=1
)

set_target_properties(${PROJECT_NAME}_operator_sqlite3 PROPERTIES
    C_STANDARD 17
    C_STANDARD_REQUIRED ON
)
