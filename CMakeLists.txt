# =============================================================================
# wsm_driver — single ESP-IDF component (WIZnet W5500/W6300 over SPI/QSPI).
# =============================================================================
# Layout mirrors wsm_driver/port/, folded into ONE registered component:
#   port/backend/           backend-neutral bring-up harness + shared socket vtables
#   port/ioLibrary_Driver/  chip driver-port code (TOE SPI transport, --wrap shim,
#                           and the vendored esp_eth W5500 MAC/PHY for the ETH path)
#   third_party/ioLibrary_Driver/  vendored WIZnet ioLibrary (git submodule)
#
# Config conventions (unchanged from wsm_driver):
#   - pins / SPI  -> component Kconfig (CONFIG_WSM_DRIVER_*), via wsm_driver_spi_config_t.
#   - network id  -> wiz_NetInfo passed to wiznet_net_init() (net_backend.h).
#
# Backend is a Kconfig CHOICE (mutually exclusive — both define wiznet_net_init):
#   CONFIG_WSM_DRIVER_BACKEND_TOE  W5500/W6300 hardware TCP/IP (ioLibrary).
#   CONFIG_WSM_DRIVER_BACKEND_ETH  W5500/W6300 as esp_eth MACRAW + software LwIP.

# --- minimum ESP-IDF ---------------------------------------------------------
# v6.0 is a hard floor: the ETH backend binds esp_eth_mac_t callbacks introduced
# after v5.4 and the TLS examples assume mbedTLS 4.0. idf_component.yml declares
# the same requirement, but that is only enforced for registry installs — a
# manual clone or git submodule reaches the build system unchecked, so fail here
# with a readable message instead of a wall of compile errors.
if(IDF_VERSION_MAJOR LESS 6)
    message(FATAL_ERROR
        "wsm_driver requires ESP-IDF v6.0 or later "
        "(found v${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}.${IDF_VERSION_PATCH})")
endif()

# --- backend-neutral sources (always built) --------------------------------
# Wi-Fi STA bring-up + the shared socket vtables examples may inject.
set(WSM_DRIVER_SRCS
    "port/backend/src/wifi_backend.c"
    "port/backend/src/net_sock_ops.c"
    "port/backend/src/net_wifi_ops.c")

if(CONFIG_WSM_DRIVER_BACKEND_ETH)
    # esp_eth path + the per-chip MAC/PHY driver. The two drivers share nothing
    # but their shape: the W5500 pair is the vendored Espressif driver (SPI VDM
    # framing, no register locks), the W6300 pair is this component's own (QSPI
    # framing, chip/net/phy unlock, PHYSR+PHYCR0/1 instead of PHYCFGR).
    list(APPEND WSM_DRIVER_SRCS "port/backend/src/net_backend_eth.c")
    if(CONFIG_WSM_DRIVER_CHIP_W6300)
        list(APPEND WSM_DRIVER_SRCS
            "port/ioLibrary_Driver/src/esp_eth_mac_w6300.c"
            "port/ioLibrary_Driver/src/esp_eth_phy_w6300.c")
    else()
        list(APPEND WSM_DRIVER_SRCS
            "port/ioLibrary_Driver/src/esp_eth_mac_w5500.c"
            "port/ioLibrary_Driver/src/esp_eth_phy_w5500.c")
    endif()
else()
    # TOE path: Kconfig-driven SPI/QSPI transport + hardware-TCP/IP bring-up.
    list(APPEND WSM_DRIVER_SRCS
        "port/backend/src/net_backend_toe.c"
        "port/ioLibrary_Driver/src/wsm_driver_spi.c")
    if(CONFIG_WSM_DRIVER_SOCKET_WRAP)
        # Opt-in BSD-socket integration: neutral TOE API + lwIP --wrap glue.
        list(APPEND WSM_DRIVER_SRCS
            "port/ioLibrary_Driver/src/wiznet_toe.c"
            "port/ioLibrary_Driver/src/wiztoe_wrap.c"
            # DHCP/DNS over the BSD socket vtable. Needs the wrap: its sockets
            # are ordinary lwip_* calls that must land on the TOE allocator.
            "port/backend/src/wsm_dhcp.c")
    endif()
endif()

# NOTE: REQUIRES / PRIV_REQUIRES must be listed UNCONDITIONALLY. ESP-IDF expands
# component requirements in an early pass where CONFIG_* values are not yet
# applied, so gating them on a CONFIG_* silently drops them. The union of both
# backends' deps is listed; the ones unused by the selected backend are harmless.
idf_component_register(
    SRCS ${WSM_DRIVER_SRCS}
    INCLUDE_DIRS
        "include"
    PRIV_INCLUDE_DIRS
        # "port/backend/inc"
        # wiznet_toe.h used to live here. It moved to include/ because a caller
        # outside the component legitimately needs wiztoe_sn_of_fd() -- see the
        # note in that header. What stays private is the transport/eth glue.
        "port/ioLibrary_Driver/inc"
    REQUIRES
        esp_driver_spi
        esp_driver_gpio
        # Public: include/esp_eth_{mac,phy}_{w5500,w6300}.h include esp_eth's own
        # public headers (esp_eth_com.h / esp_eth_mac_spi.h / esp_eth_phy.h), so
        # consumers of those headers need esp_eth on their include path too.
        esp_eth
    PRIV_REQUIRES
        esp_netif
        esp_event
        esp_timer
        lwip
        esp_wifi
        nvs_flash
        driver
        spi_flash
)

target_compile_features(${COMPONENT_LIB} PUBLIC c_std_11)

# net_wifi_ops.c (and the example's wifi_loopback.c) select __real_lwip_* vs
# plain lwip_* via the WSM_DRIVER_SOCKET_WRAP macro. __real_lwip_* exist only
# when --wrap is active, so the macro tracks CONFIG_WSM_DRIVER_SOCKET_WRAP —
# NOT the backend choice.
#
# PUBLIC (not PRIVATE): this component declares the --wrap linker flags below, so
# it is also the single source of truth for "is the wrap active?". PUBLIC makes
# the macro propagate to every component that REQUIRES wsm_driver, so examples
# must NOT define it themselves — a hardcoded copy in an example silently breaks
# the ETH backend build, where SOCKET_WRAP cannot be set and __real_lwip_* do not
# exist.
if(CONFIG_WSM_DRIVER_SOCKET_WRAP)
    target_compile_definitions(${COMPONENT_LIB} PUBLIC WSM_DRIVER_SOCKET_WRAP=1)
else()
    target_compile_definitions(${COMPONENT_LIB} PUBLIC WSM_DRIVER_SOCKET_WRAP=0)
endif()

if(CONFIG_WSM_DRIVER_CHIP_W5500)
    set(WSM_DRIVER_CHIP_NAME W5500)
    set(WSM_DRIVER_CHIP_SOURCE "${COMPONENT_DIR}/third_party/ioLibrary_Driver/Ethernet/W5500/w5500.c")
    set(WSM_DRIVER_CHIP_INCLUDE "${COMPONENT_DIR}/third_party/ioLibrary_Driver/Ethernet/W5500")
elseif(CONFIG_WSM_DRIVER_CHIP_W6300)
    set(WSM_DRIVER_CHIP_NAME W6300)
    set(WSM_DRIVER_CHIP_SOURCE "${COMPONENT_DIR}/third_party/ioLibrary_Driver/Ethernet/W6300/w6300.c")
    set(WSM_DRIVER_CHIP_INCLUDE "${COMPONENT_DIR}/third_party/ioLibrary_Driver/Ethernet/W6300")
else()
    message(FATAL_ERROR "No supported WIZnet chip selected")
endif()

target_compile_definitions(${COMPONENT_LIB} PUBLIC
    _WIZCHIP_=${WSM_DRIVER_CHIP_NAME}
    # _PHY_IO_MODE_=_PHY_IO_MODE_PHYCR_
)

if(CONFIG_WSM_DRIVER_CHIP_W5500)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE WIZNET_TOE_W5500=1)
endif()

if(CONFIG_WSM_DRIVER_CHIP_W6300)
    # W6300 talks QSPI frames (opcode + 16-bit address + dummy + data); the
    # mode bits below are embedded into every opcode by w6300.c.
    if(CONFIG_WSM_DRIVER_QSPI_QUAD)
        target_compile_definitions(${COMPONENT_LIB} PUBLIC _WIZCHIP_QSPI_MODE_=QSPI_QUAD_MODE)
    else()
        target_compile_definitions(${COMPONENT_LIB} PUBLIC _WIZCHIP_QSPI_MODE_=QSPI_SINGLE_MODE)
    endif()
endif()

set(WIZNET_IOLIB_PATH "${COMPONENT_DIR}/third_party/ioLibrary_Driver")

if(EXISTS "${WIZNET_IOLIB_PATH}/Ethernet/socket.c")
    set(WSM_DRIVER_IOLIB_SOURCES
        "${WIZNET_IOLIB_PATH}/Application/loopback/loopback.c"
        "${WIZNET_IOLIB_PATH}/Application/multicast/multicast.c"
        "${WIZNET_IOLIB_PATH}/Ethernet/socket.c"
        "${WIZNET_IOLIB_PATH}/Ethernet/wizchip_conf.c"
        "${WSM_DRIVER_CHIP_SOURCE}"
        "${WIZNET_IOLIB_PATH}/Internet/DHCP/dhcp.c"
        "${WIZNET_IOLIB_PATH}/Internet/DNS/dns.c"
        "${WIZNET_IOLIB_PATH}/Internet/SNTP/sntp.c"
        "${WIZNET_IOLIB_PATH}/Internet/TFTP/tftp.c"
        "${WIZNET_IOLIB_PATH}/Internet/TFTP/netutil.c"
        "${WIZNET_IOLIB_PATH}/Internet/httpServer/httpParser.c"
        "${WIZNET_IOLIB_PATH}/Internet/httpServer/httpServer.c"
        "${WIZNET_IOLIB_PATH}/Internet/httpServer/httpUtil.c"
        "${WIZNET_IOLIB_PATH}/Internet/MQTT/MQTTClient.c"
        "${WIZNET_IOLIB_PATH}/Internet/MQTT/mqtt_interface.c"
        "${WIZNET_IOLIB_PATH}/Internet/MQTT/MQTTPacket/src/MQTTConnectClient.c"
        "${WIZNET_IOLIB_PATH}/Internet/MQTT/MQTTPacket/src/MQTTConnectServer.c"
        "${WIZNET_IOLIB_PATH}/Internet/MQTT/MQTTPacket/src/MQTTDeserializePublish.c"
        "${WIZNET_IOLIB_PATH}/Internet/MQTT/MQTTPacket/src/MQTTFormat.c"
        "${WIZNET_IOLIB_PATH}/Internet/MQTT/MQTTPacket/src/MQTTPacket.c"
        "${WIZNET_IOLIB_PATH}/Internet/MQTT/MQTTPacket/src/MQTTSerializePublish.c"
        "${WIZNET_IOLIB_PATH}/Internet/MQTT/MQTTPacket/src/MQTTSubscribeClient.c"
        "${WIZNET_IOLIB_PATH}/Internet/MQTT/MQTTPacket/src/MQTTSubscribeServer.c"
        "${WIZNET_IOLIB_PATH}/Internet/MQTT/MQTTPacket/src/MQTTUnsubscribeClient.c"
        "${WIZNET_IOLIB_PATH}/Internet/MQTT/MQTTPacket/src/MQTTUnsubscribeServer.c"
    )

    target_include_directories(${COMPONENT_LIB} PUBLIC
        "${WIZNET_IOLIB_PATH}"
        "${WIZNET_IOLIB_PATH}/Application/loopback"
        "${WIZNET_IOLIB_PATH}/Application/multicast"
        "${WIZNET_IOLIB_PATH}/Ethernet"
        "${WSM_DRIVER_CHIP_INCLUDE}"
        "${WIZNET_IOLIB_PATH}/Internet"
        "${WIZNET_IOLIB_PATH}/Internet/DHCP"
        "${WIZNET_IOLIB_PATH}/Internet/DNS"
        "${WIZNET_IOLIB_PATH}/Internet/SNTP"
        "${WIZNET_IOLIB_PATH}/Internet/TFTP"
        "${WIZNET_IOLIB_PATH}/Internet/httpServer"
        "${WIZNET_IOLIB_PATH}/Internet/MQTT"
        "${WIZNET_IOLIB_PATH}/Internet/MQTT/MQTTPacket/src"
    )

    target_sources(${COMPONENT_LIB} PRIVATE ${WSM_DRIVER_IOLIB_SOURCES})

    # Keep strict warnings for project code, but silence the bundled third-party
    # code entirely. A plain -Wno-error is not enough here: ESP-IDF promotes
    # specific diagnostics with -Werror=format / -Werror=parentheses, which a
    # general -Wno-error does NOT undo (GCC keeps explicit -Werror=foo). The
    # vendored ioLibrary trips those (e.g. %d with int32_t, && within ||), so
    # suppress all warnings on these sources with -w.
    set_source_files_properties(${WSM_DRIVER_IOLIB_SOURCES} PROPERTIES COMPILE_OPTIONS "-w")
else()
    message(FATAL_ERROR "ioLibrary_Driver submodule not found at ${WIZNET_IOLIB_PATH}. Run: git submodule update --init --recursive")
endif()

# =============================================================================
# BSD socket integration (CONFIG_WSM_DRIVER_SOCKET_WRAP) — TOE backend only
# =============================================================================
# Redirect lwIP's BSD socket entry points to the WIZnet hardware sockets so that
# standard socket code runs over the TOE unchanged. Everything here is scoped to
# the opt-in build only, so the direct-ioLibrary examples are unaffected.
if(CONFIG_WSM_DRIVER_SOCKET_WRAP)
    # ioLibrary's Ethernet/socket.c defines a global close(uint8_t) that would
    # hijack newlib's POSIX close() and break the VFS -> lwip_close -> __wrap
    # path. Rename close -> wiz_close CONSISTENTLY across every ioLibrary TU that
    # references it (they call each other's close), so ioLibrary stays internally
    # consistent while POSIX close() is left to newlib/VFS. (The bundled sources
    # already carry COMPILE_OPTIONS "-w" from above; add the definition here.)
    set_source_files_properties(${WSM_DRIVER_IOLIB_SOURCES} PROPERTIES
        COMPILE_DEFINITIONS "close=wiz_close")

    # wiznet_toe.c is the only main TU that includes the ioLibrary "socket.h".
    #  - close=wiz_close: match the rename applied to ioLibrary's socket.c.
    #  - -iquote: make `#include "socket.h"` resolve to ioLibrary's
    #    Ethernet/socket.h (searched before -I dirs for quote-form includes),
    #    not lwIP's sys/socket.h.
    set_source_files_properties("port/ioLibrary_Driver/src/wiznet_toe.c" PROPERTIES
        COMPILE_DEFINITIONS "close=wiz_close"
        COMPILE_OPTIONS "-iquote;${WIZNET_IOLIB_PATH}/Ethernet")

    # Intercept lwIP's BSD socket entry points at link time -> wiztoe_wrap.c.
    # INTERFACE so the flags reach the final executable link; -u forces the
    # __wrap_* symbols to be kept even if only referenced through the wrap.
    # read/write/readv/writev/fcntl/shutdown/getpeername are as much a part of the
    # BSD socket vocabulary as recv/send. Without them those calls reach the real
    # lwIP, which holds no socket for a TOE fd, and fail with EBADF at run time
    # only -- the build stays clean, which makes it the worst kind of gap.
    # lwip_ioctl is deliberately absent: no confirmed caller needs it yet, and
    # wiztoe_available() is ready if FIONREAD ever does.
    set(WRAP_FNS lwip_socket lwip_bind lwip_listen lwip_accept lwip_connect
                 lwip_send lwip_recv lwip_recvfrom lwip_sendto
                 lwip_setsockopt lwip_getsockopt lwip_getsockname lwip_close
                 lwip_read lwip_write lwip_readv lwip_writev
                 lwip_fcntl lwip_shutdown lwip_getpeername)
    foreach(fn ${WRAP_FNS})
        target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=${fn}")
        target_link_libraries(${COMPONENT_LIB} INTERFACE "-u __wrap_${fn}")
    endforeach()
endif()
