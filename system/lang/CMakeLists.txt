idf_component_register(
    SRCS "lang.c" "strings_zh_cn.c" "strings_en.c" "countries.c"
    INCLUDE_DIRS "."
    REQUIRES flash_store
)
