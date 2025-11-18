idf_component_register(SRCS "paddle_hal.cpp"
                             "high_precision_clock.cpp"
                             "tx_hal.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES driver esp_timer)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
