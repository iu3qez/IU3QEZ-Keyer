idf_component_register(
    SRCS
        "dns_server.cpp"
        "minimal_http_server.cpp"
        "setup_page.cpp"
        "captive_portal_manager.cpp"
    INCLUDE_DIRS
        "include"
    REQUIRES
        lwip
        esp_http_server
        wifi_subsystem
        config
        diagnostics_subsystem
        ui  # HttpServer for port 80 coordination
        json  # cJSON for JSON serialization/parsing
)
