cmake_minimum_required(VERSION 3.16)

# ioLibrary_Driver submodule root
set(IOLIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ioLibrary_Driver")

# ioLibrary source files required for W5500 TOE direct socket usage
set(IOLIB_SRCS
    "${IOLIB_DIR}/Ethernet/W5500/w5500.c"
    "${IOLIB_DIR}/Ethernet/socket.c"
    "${IOLIB_DIR}/Ethernet/wizchip_conf.c"
)

# Component sources
set(COMPONENT_SRCS
    "src/esp_w5500_toe.c"
    "src/esp_w5500_toe_spi.c"
    "src/esp_w5500_toe_socket.c"
    ${IOLIB_SRCS}
)

set(COMPONENT_INCLUDE_DIRS
    "include"
    "${IOLIB_DIR}/Ethernet"
    "${IOLIB_DIR}/Ethernet/W5500"
)

idf_component_register(
    SRCS          ${COMPONENT_SRCS}
    INCLUDE_DIRS  ${COMPONENT_INCLUDE_DIRS}
    REQUIRES      driver freertos esp_log
)

# Suppress ioLibrary warnings that are not relevant to our build
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-unused-variable
    -Wno-unused-function
)
