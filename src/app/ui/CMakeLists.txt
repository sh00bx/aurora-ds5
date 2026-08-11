target_sources(moonlight-lib PRIVATE
        root.c
        ui_input.c
        fatal_error.c
        launcher/launcher.view.c
        launcher/launcher.controller.c
        launcher/apps.controller.c
        launcher/add.dialog.c
        launcher/pair.dialog.c
        launcher/appitem.view.c
        launcher/server.context_menu.c
        launcher/server.popup.c
        launcher/coverloader.c
        streaming/streaming.view.c
        streaming/streaming.controller.c
        streaming/controller_info.c
        streaming/soft_keyboard.c
        streaming/hints.c
        help/help.dialog.c
        common/progress_dialog.c
        )
if (TARGET_WEBOS)
    target_sources(moonlight-lib PRIVATE
            streaming/hid_passthrough_panel.c
            streaming/hid_pt_panel_model.c
            )
endif()
add_subdirectory(settings)