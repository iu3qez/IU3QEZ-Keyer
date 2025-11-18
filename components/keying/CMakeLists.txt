idf_component_register(SRCS "paddle_engine.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES keyer_hal timeline)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
