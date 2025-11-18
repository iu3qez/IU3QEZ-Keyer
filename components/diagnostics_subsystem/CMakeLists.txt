idf_component_register(
    SRCS "diagnostics_subsystem.cpp"
         "led_driver.cpp"
    INCLUDE_DIRS "include"
    REQUIRES config keyer_hal timeline freertos esp_timer led_strip
)
