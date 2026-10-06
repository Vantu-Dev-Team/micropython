include(boards/mpconfigboard_esp32_common.cmake)

list(APPEND SDKCONFIG_DEFAULTS
    boards/sdkconfig.spiram_esp32
    boards/PYCOM_LOPY4/sdkconfig.board
)
