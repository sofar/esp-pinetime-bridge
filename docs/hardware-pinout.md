# Waveshare ESP32-S3-Touch-AMOLED-1.8 Hardware Reference

## Board Specifications
- MCU: ESP32-S3R8 (dual-core LX7, 240MHz)
- Display: 1.8" AMOLED, 368x448, QSPI (SH8601)
- Touch: FT3168 (I2C, ft5x06 compatible)
- Audio Codec: ES8311 (I2S DAC for speaker)
- Microphone: ES7210 (I2S ADC)
- IMU: 6-axis (QMI8658)
- RTC: PCF85063
- Power: AXP2101 PMIC
- I/O Expander: TCA9554 (I2C 0x20)

## GPIO Pin Assignments

### Display (QSPI — handled by ESPHome model driver)
| Function | GPIO |
|----------|------|
| CS | GPIO12 |
| SCLK | GPIO11 |
| SDIO0 | GPIO4 |
| SDIO1 | GPIO5 |
| SDIO2 | GPIO6 |
| SDIO3 | GPIO7 |

### Touch (I2C)
| Function | GPIO |
|----------|------|
| SDA | GPIO15 |
| SCL | GPIO14 |
| Reset | TCA9554 pin 0 |

### Audio — ES8311 Codec (I2S + I2C)
| Function | GPIO |
|----------|------|
| I2S BCLK | GPIO9 |
| I2S LRCLK/WS | GPIO45 |
| I2S DOUT (speaker) | GPIO8 |
| I2S DIN (microphone) | GPIO10 |
| I2S MCLK | GPIO16 |
| I2C SDA | GPIO15 (shared) |
| I2C SCL | GPIO14 (shared) |

Note: Speaker amplifier is powered from the AXP2101 PMIC (rails enabled by default) and driven by the ES8311 codec itself — no ESP32 GPIO gates the PA. GPIO46 is a strapping pin with an external pull-down and is NOT the PA enable, despite appearing so in some earlier notes.

### I/O Expander (TCA9554 at I2C 0x20)
| Pin | Function |
|-----|----------|
| P0 | Touch reset |
| P1 | Display reset |
| P2 | Unknown |

### SD Card
| Function | GPIO |
|----------|------|
| CLK | GPIO11 |
| CMD | GPIO13 |
| D0 | GPIO44 |

### USB
| Function | GPIO |
|----------|------|
| USB D+ | GPIO20 |
| USB D- | GPIO19 |

## Sources
- [Waveshare Wiki](https://www.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-1.8)
- [Schematic PDF](https://files.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-1.8/ESP32-S3-Touch-AMOLED-1.8.pdf) (saved as ESP32-S3-Touch-AMOLED-1.8-schematic.pdf)
- [ESPHome device page (1.75" variant, same pinout)](https://devices.esphome.io/devices/waveshare-esp32-s3-touch-amoled-1.75/)
- [CNX Software review](https://www.cnx-software.com/2025/01/13/fully-enclosed-esp32-s3-board-features-1-8-inch-amoled-microphone-speaker-for-ai-audio-applications/)
