# Vendored esp_websocket_client (modified monolith copy). Sources live under smart_common/.
set(SMART_COMMON_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../components/smart_common")

idf_component_register(
        SRCS "${SMART_COMMON_ROOT}/src/esp_websocket_client.c"
        INCLUDE_DIRS "${SMART_COMMON_ROOT}/include"
        REQUIRES lwip esp-tls tcp_transport http_parser esp_event
        PRIV_REQUIRES esp_timer
)
