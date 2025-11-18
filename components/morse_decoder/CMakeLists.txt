# Morse Decoder Component
# Adaptive morse code recognition with WPM detection

idf_component_register(
    SRCS
        "adaptive_timing_classifier.cpp"
        "morse_table.cpp"
        "morse_decoder.cpp"
        "morse_encoder.cpp"
    INCLUDE_DIRS
        "include"
    REQUIRES
        keyer_hal
        timeline
)

# Require C++17 for std::unordered_map, std::mutex, etc.
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
