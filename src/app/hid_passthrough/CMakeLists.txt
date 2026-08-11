if (TARGET_WEBOS)
    target_sources(moonlight-lib PRIVATE
            ctm_hid.c
            enet_transport.c
            ctm_transport.c
            hid_pt_device_prefs.c
            hid_pt_gamepad_match.c
            hid_passthrough_manager.c
            ctm/ctm_state.c
            ctm/ctm_classify.c
            ctm/ctm_dev_composite.c
            ctm/ctm_enumerate.c
            ctm/ctm_model.c
            ctm/ui_bridge.c
            ctm/controllers/controller_common.c
            ctm/controllers/ctm_composite.c
            ctm/controllers/ctm_feature_worker.c
            ctm/controllers/ctm_hid_io.c
            ctm/controllers/ctm_xpad_evdev.c
            ctm/controllers/ctm_ds5_audio.c
            ctm/controllers/ds5_acl_tx.c
            ctm/controllers/ds5_hidfd.c
            ctm/controllers/controller_ds4.c
            ctm/controllers/controller_ds5.c
            ctm/controllers/controller_generic.c
            ctm/controllers/controller_steam_puck.c
            ctm/controllers/controller_flydigi.c
            ctm/controllers/controller_xbox.c)
    target_include_directories(moonlight-lib PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}/ctm
            ${CMAKE_CURRENT_SOURCE_DIR}/ctm/controllers)
    target_link_libraries(moonlight-lib PRIVATE enet pthread)
endif()
