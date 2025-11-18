idf_component_register(
    SRCS "keying_subsystem.cpp"
    INCLUDE_DIRS "include"
    REQUIRES config keying timeline keyer_hal freertos morse_decoder
    PRIV_REQUIRES audio_subsystem diagnostics_subsystem remote
)
