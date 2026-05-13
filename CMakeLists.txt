idf_component_register(
    SRCS
        "src/esp_wiz_toe.c"
        "src/esp_wiz_toe_port.c"
        "src/esp_wiz_toe_spi.c"
        "src/esp_wiz_toe_socket.c"
        "src/esp_wiz_toe_net.c"
    INCLUDE_DIRS
        "include"
    PRIV_INCLUDE_DIRS
        "src"
    REQUIRES
        driver
)

target_compile_features(${COMPONENT_LIB} PUBLIC c_std_11)
target_compile_definitions(${COMPONENT_LIB} PRIVATE WIZNET_TOE_W5500=1)

set(WIZNET_IOLIB_PATH "${COMPONENT_DIR}/third_party/ioLibrary_Driver")

if(EXISTS "${WIZNET_IOLIB_PATH}/Ethernet/socket.c")
    target_include_directories(${COMPONENT_LIB} PRIVATE
        "${WIZNET_IOLIB_PATH}/Ethernet"
        "${WIZNET_IOLIB_PATH}/Ethernet/W5500"
        "${WIZNET_IOLIB_PATH}/Internet"
        "${WIZNET_IOLIB_PATH}/Internet/DHCP"
        "${WIZNET_IOLIB_PATH}/Internet/DNS"
    )

    target_sources(${COMPONENT_LIB} PRIVATE
        "${WIZNET_IOLIB_PATH}/Ethernet/socket.c"
        "${WIZNET_IOLIB_PATH}/Ethernet/wizchip_conf.c"
        "${WIZNET_IOLIB_PATH}/Ethernet/W5500/w5500.c"
        "${WIZNET_IOLIB_PATH}/Internet/DHCP/dhcp.c"
        "${WIZNET_IOLIB_PATH}/Internet/DNS/dns.c"
    )
else()
    message(STATUS "ioLibrary_Driver submodule not found yet at ${WIZNET_IOLIB_PATH}")
endif()
