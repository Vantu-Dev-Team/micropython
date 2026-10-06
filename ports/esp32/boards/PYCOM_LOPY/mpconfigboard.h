// Both of these can be set by mpconfigboard.cmake if a BOARD_VARIANT is
// specified.

#ifndef MICROPY_HW_BOARD_NAME
#define MICROPY_HW_BOARD_NAME "Pycom LoPy"
#endif

#ifndef MICROPY_HW_MCU_NAME
#define MICROPY_HW_MCU_NAME "ESP32"
#endif

#define MICROPY_PY_NETWORK_HOSTNAME_DEFAULT "mpy-lopy"

// Match the Pycom firmware defaults.  The ESP32 port defaults would place
// UART1, UART2 and I2C0 on the flash, PSRAM or LoRa SPI pins of this module.
#define MICROPY_HW_UART1_TX (4)  // P3
#define MICROPY_HW_UART1_RX (15) // P4
#define MICROPY_HW_UART2_TX (2)  // P8
#define MICROPY_HW_UART2_RX (12) // P9
#define MICROPY_HW_I2C0_SCL (13) // P10
#define MICROPY_HW_I2C0_SDA (12) // P9
