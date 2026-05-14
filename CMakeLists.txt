idf_component_register(
    SRCS
        "src/esp_wiz_toe_spi.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        esp_driver_spi
        esp_driver_gpio
)

target_compile_features(${COMPONENT_LIB} PUBLIC c_std_11)

if(CONFIG_ESP_WIZ_TOE_CHIP_W5500)
    set(ESP_WIZ_TOE_CHIP_NAME W5500)
    set(ESP_WIZ_TOE_CHIP_SOURCE "${COMPONENT_DIR}/third_party/ioLibrary_Driver/Ethernet/W5500/w5500.c")
    set(ESP_WIZ_TOE_CHIP_INCLUDE "${COMPONENT_DIR}/third_party/ioLibrary_Driver/Ethernet/W5500")
else()
    message(FATAL_ERROR "No supported WIZnet chip selected")
endif()

target_compile_definitions(${COMPONENT_LIB} PUBLIC
    _WIZCHIP_=${ESP_WIZ_TOE_CHIP_NAME}
    _PHY_IO_MODE_=_PHY_IO_MODE_PHYCR_
)

if(CONFIG_ESP_WIZ_TOE_CHIP_W5500)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE WIZNET_TOE_W5500=1)
endif()

set(WIZNET_IOLIB_PATH "${COMPONENT_DIR}/third_party/ioLibrary_Driver")

if(EXISTS "${WIZNET_IOLIB_PATH}/Ethernet/socket.c")
    set(ESP_WIZ_TOE_IOLIB_SOURCES
        "${WIZNET_IOLIB_PATH}/Application/loopback/loopback.c"
        "${WIZNET_IOLIB_PATH}/Ethernet/socket.c"
        "${WIZNET_IOLIB_PATH}/Ethernet/wizchip_conf.c"
        "${ESP_WIZ_TOE_CHIP_SOURCE}"
        "${WIZNET_IOLIB_PATH}/Internet/DHCP/dhcp.c"
        "${WIZNET_IOLIB_PATH}/Internet/DNS/dns.c"
    )

    target_include_directories(${COMPONENT_LIB} PUBLIC
        "${WIZNET_IOLIB_PATH}"
        "${WIZNET_IOLIB_PATH}/Application/loopback"
        "${WIZNET_IOLIB_PATH}/Ethernet"
        "${ESP_WIZ_TOE_CHIP_INCLUDE}"
        "${WIZNET_IOLIB_PATH}/Internet"
        "${WIZNET_IOLIB_PATH}/Internet/DHCP"
        "${WIZNET_IOLIB_PATH}/Internet/DNS"
    )

    target_sources(${COMPONENT_LIB} PRIVATE ${ESP_WIZ_TOE_IOLIB_SOURCES})

    # Keep strict warnings for project code, but relax warning-as-error for bundled third-party code.
    set_source_files_properties(${ESP_WIZ_TOE_IOLIB_SOURCES} PROPERTIES COMPILE_OPTIONS "-Wno-error")
    set_source_files_properties("${WIZNET_IOLIB_PATH}/Ethernet/socket.c" PROPERTIES COMPILE_OPTIONS "-Wno-error=misleading-indentation")
else()
    message(FATAL_ERROR "ioLibrary_Driver submodule not found at ${WIZNET_IOLIB_PATH}. Run: git submodule update --init --recursive")
endif()
