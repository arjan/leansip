find_package(Qt6 REQUIRED COMPONENTS Test)

# Each test is its own executable so failures are isolated and ctest can run them in parallel.
function(leansip_add_test name)
    cmake_parse_arguments(ARG "" "" "LIBS" ${ARGN})
    # The fake server header carries Q_OBJECT; listing it lets automoc pick it up.
    qt_add_executable(${name} ${name}.cpp fake_baresip_server.h)
    target_link_libraries(${name} PRIVATE leansip_core Qt6::Test leansip_warnings ${ARG_LIBS})
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endfunction()

leansip_add_test(tst_netstring)
leansip_add_test(tst_accounts_parser)
leansip_add_test(tst_settings)
leansip_add_test(tst_config_deriver)
leansip_add_test(tst_dtmf_tone)
leansip_add_test(tst_baresip_event)
leansip_add_test(tst_baresip_client)
leansip_add_test(tst_baresip_process)
leansip_add_test(tst_call_controller)
leansip_add_test(tst_single_instance)
leansip_add_test(tst_ctrl_proxy)
leansip_add_test(tst_media)
leansip_add_test(tst_call_audio)
leansip_add_test(tst_elevenlabs)
leansip_add_test(tst_unix_signals)
leansip_add_test(tst_main_window LIBS leansip_ui)

# End to end with a real baresip on the loopback interface; skipped (77) without baresip.
add_test(NAME integration_loopback
         COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/integration_loopback.sh $<TARGET_FILE_DIR:leansipctl>)
set_tests_properties(integration_loopback PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 180)
