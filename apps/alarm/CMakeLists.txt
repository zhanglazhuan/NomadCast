idf_component_register(
    SRCS
        "app.c" "model.c" "view.c" "controller.c"
        "subpages/view_alarm_list.c" "subpages/view_alarm_edit.c"
    INCLUDE_DIRS "." "subpages"
    REQUIRES lvgl app_manager uilv lang alarm_service
)
