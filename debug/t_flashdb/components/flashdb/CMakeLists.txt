# FlashDB (vendored at repo-root vendor/FlashDB) — file-mode (POSIX) build.
# Only the KVDB + generic + file-backend sources are needed (no FAL/flash port).
# fdb_cfg.h in this dir selects FDB_USING_KVDB + FDB_USING_FILE_POSIX_MODE.
# Normalize the ../ path to a clean absolute path (some IDF versions reject
# non-normalized SRCS paths).
get_filename_component(FDB_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../../vendor/FlashDB" ABSOLUTE)

idf_component_register(
    SRCS
        "${FDB_ROOT}/src/fdb.c"
        "${FDB_ROOT}/src/fdb_kvdb.c"
        "${FDB_ROOT}/src/fdb_tsdb.c"
        "${FDB_ROOT}/src/fdb_utils.c"
        "${FDB_ROOT}/src/fdb_file.c"
    INCLUDE_DIRS
        "${FDB_ROOT}/inc"
        "."
)
