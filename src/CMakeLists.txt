# Core: no GUI dependency, fully unit-testable.
qt_add_library(leansip_core STATIC
    core/netstring.cpp
    core/netstring.h
    core/accounts_parser.cpp
    core/accounts_parser.h
    core/account_validator.cpp
    core/account_validator.h
    core/config_deriver.cpp
    core/config_deriver.h
    core/dtmf_tone.cpp
    core/dtmf_tone.h
    core/baresip_event.cpp
    core/baresip_event.h
    core/baresip_client.cpp
    core/baresip_client.h
    core/baresip_backend.h
    core/baresip_process.cpp
    core/baresip_process.h
    core/call_controller.cpp
    core/call_controller.h
    core/single_instance.cpp
    core/single_instance.h
    core/unix_signals.cpp
    core/unix_signals.h
)
target_include_directories(leansip_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(leansip_core PUBLIC Qt6::Core Qt6::Network PRIVATE leansip_warnings)

qt_add_library(leansip_ui STATIC
    ui/accounts_dialog.cpp
    ui/accounts_dialog.h
    ui/main_window.cpp
    ui/main_window.h
)
target_link_libraries(leansip_ui PUBLIC leansip_core Qt6::Widgets PRIVATE leansip_warnings)

qt_add_executable(leansip main.cpp)
target_link_libraries(leansip PRIVATE leansip_ui leansip_warnings)

install(TARGETS leansip RUNTIME DESTINATION bin)
