# Core: no GUI dependency, fully unit-testable.
qt_add_library(leansip_core STATIC
    core/netstring.cpp
    core/netstring.h
    core/accounts_parser.cpp
    core/accounts_parser.h
    core/config_deriver.cpp
    core/config_deriver.h
    core/dtmf_tone.cpp
    core/dtmf_tone.h
    core/baresip_event.cpp
    core/baresip_event.h
    core/baresip_client.cpp
    core/baresip_client.h
    core/baresip_process.cpp
    core/baresip_process.h
    core/call_controller.cpp
    core/call_controller.h
    core/single_instance.cpp
    core/single_instance.h
    core/unix_signals.cpp
    core/unix_signals.h
    core/profile.cpp
    core/profile.h
    core/settings.cpp
    core/settings.h
    core/ctrl_proxy.cpp
    core/ctrl_proxy.h
    core/daemon_client.cpp
    core/daemon_client.h
    core/wav.cpp
    core/wav.h
    core/speech_segmenter.cpp
    core/speech_segmenter.h
    core/media_control.cpp
    core/media_control.h
    core/elevenlabs.cpp
    core/elevenlabs.h
    core/log_file.cpp
    core/log_file.h
    core/call_analysis.cpp
    core/call_analysis.h
    core/call_audio.cpp
    core/call_audio.h
)
target_include_directories(leansip_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(leansip_core PUBLIC Qt6::Core Qt6::Network PRIVATE leansip_warnings)
target_include_directories(leansip_core SYSTEM PRIVATE ${TOMLPLUSPLUS_INCLUDE_DIR})
target_compile_definitions(leansip_core PUBLIC LEANSIP_VERSION="${PROJECT_VERSION}")

qt_add_library(leansip_ui STATIC
    ui/main_window.cpp
    ui/main_window.h
    ui/waveform_view.cpp
    ui/waveform_view.h
)
target_link_libraries(leansip_ui PUBLIC leansip_core Qt6::Widgets PRIVATE leansip_warnings)

qt_add_executable(leansip main.cpp)
target_link_libraries(leansip PRIVATE leansip_ui leansip_warnings)

# The daemon that owns baresip, and the command-line client for it. Built next to the dialer,
# which looks for leansipd in its own directory first.
qt_add_executable(leansipd daemon/main.cpp)
target_link_libraries(leansipd PRIVATE leansip_core leansip_warnings)

qt_add_executable(leansipctl leansipctl/main.cpp)
target_link_libraries(leansipctl PRIVATE leansip_core leansip_warnings)

install(TARGETS leansip leansipd leansipctl RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
