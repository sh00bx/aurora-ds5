# The Pseudo-VRR scheduler is pure C with no platform calls, so its test links only
# Unity and the one source file — it builds and runs without the rest of moonlight-lib.
add_executable(test_vrr_timing test_vrr_timing.c ${CMAKE_SOURCE_DIR}/src/app/stream/video/vrr_timing.c)
target_include_directories(test_vrr_timing PRIVATE ${CMAKE_SOURCE_DIR}/src/app)
target_link_libraries(test_vrr_timing PRIVATE unity m)
add_test(test_vrr_timing test_vrr_timing)
