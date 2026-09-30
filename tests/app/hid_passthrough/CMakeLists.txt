# The per-controller settings codec is pure C with no platform calls, so its test links
# only Unity and the one source file, like test_vrr_timing.
add_executable(test_pad_settings test_pad_settings.c ${CMAKE_SOURCE_DIR}/src/app/hid_passthrough/hid_pt_pad_settings.c)
target_include_directories(test_pad_settings PRIVATE ${CMAKE_SOURCE_DIR}/src/app/hid_passthrough)
target_link_libraries(test_pad_settings PRIVATE unity)
add_test(test_pad_settings test_pad_settings)
