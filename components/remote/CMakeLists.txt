idf_component_register(
    SRCS
        "remote_cw_client.cpp"
        "remote_cw_server.cpp"
    INCLUDE_DIRS
        "include"
    REQUIRES
        lwip
        esp_timer
        keyer_hal
        audio_subsystem
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
