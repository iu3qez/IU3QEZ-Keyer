# Text Keyer Component
# Converts text to morse code and keys transmitter

idf_component_register(
    SRCS
        "text_keyer.cpp"
    INCLUDE_DIRS
        "include"
    REQUIRES
        keyer_hal
        audio_subsystem
        morse_decoder
)

# Require C++17 for std::mutex, std::lock_guard
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
