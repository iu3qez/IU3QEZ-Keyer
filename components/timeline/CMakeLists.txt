idf_component_register(SRCS "timeline_event_emitter.cpp" "event_logger.cpp"
                       INCLUDE_DIRS "include")

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
