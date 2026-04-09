# License

This project is licensed under the **GNU General Public License v3.0 or later** (GPL-3.0-or-later).

See [LICENSE](LICENSE) for the full license text.

Copyright (C) 2024-2026 Auke Kok

## Third-Party Licenses

This project incorporates or depends on the following third-party software and assets. All are compatible with GPL-3.0-or-later.

### Runtime Dependencies

#### Go Server (`server/`)

| Dependency | License | Source |
|-----------|---------|--------|
| modernc.org/sqlite v1.48.0 | BSD 3-Clause | https://github.com/modernc-org/sqlite |
| modernc.org/libc v1.70.0 | BSD 3-Clause | https://github.com/modernc-org/libc |
| modernc.org/mathutil v1.7.1 | BSD 3-Clause | https://github.com/modernc-org/mathutil |
| modernc.org/memory v1.11.0 | BSD 3-Clause | https://github.com/modernc-org/memory |
| github.com/dustin/go-humanize v1.0.1 | MIT | https://github.com/dustin/go-humanize |
| github.com/google/uuid v1.6.0 | BSD 3-Clause | https://github.com/google/uuid |
| github.com/mattn/go-isatty v0.0.20 | MIT | https://github.com/mattn/go-isatty |
| github.com/ncruces/go-strftime v1.0.0 | MIT | https://github.com/ncruces/go-strftime |
| github.com/remyoudompheng/bigfft | Apache 2.0 | https://github.com/remyoudompheng/bigfft |
| golang.org/x/sys v0.42.0 | BSD 3-Clause | https://github.com/golang/sys |

#### ESPHome Bridge (`esphome/`)

| Dependency | License | Source |
|-----------|---------|--------|
| ESPHome | GPL 3.0 | https://github.com/esphome/esphome |
| ESP-IDF (Espressif IoT Development Framework) | Apache 2.0 | https://github.com/espressif/esp-idf |
| LVGL (Light and Versatile Graphics Library) | MIT | https://github.com/lvgl/lvgl |
| NimBLE (Apache Mynewt BLE stack) | Apache 2.0 | https://github.com/apache/mynewt-nimble |

#### Fonts and Icons (`esphome/fonts/`)

| Asset | License | Source |
|-------|---------|--------|
| Material Design Icons (materialdesignicons-webfont.ttf) | Apache 2.0 | https://github.com/Templarian/MaterialDesign-Webfont |
| Roboto (Google Fonts) | Apache 2.0 | https://fonts.google.com/specimen/Roboto |

### Build-Time Dependencies

| Dependency | License | Purpose |
|-----------|---------|---------|
| Go compiler (1.25+) | BSD 3-Clause | Build server binary |
| ARM GCC (arm-none-eabi-gcc) | GPL 3.0 (with runtime exception) | Compile watch firmware |
| CMake | BSD 3-Clause | Build system for watch firmware |
| Python 3 | PSF License | Build tools and scripts |
| adafruit-nrfutil | MIT | Generate DFU packages |

### GitHub Actions (`.github/workflows/`)

| Action | License | Source |
|--------|---------|--------|
| actions/checkout@v4 | MIT | https://github.com/actions/checkout |
| actions/configure-pages@v5 | MIT | https://github.com/actions/configure-pages |
| actions/upload-pages-artifact@v3 | MIT | https://github.com/actions/upload-pages-artifact |
| actions/deploy-pages@v4 | MIT | https://github.com/actions/deploy-pages |

### Nordic DFU Protocol

The DFU client (`esphome/components/pinetime_bridge/dfu_client.cpp`) implements the Nordic Legacy DFU protocol. The implementation is original; the protocol is publicly documented by Nordic Semiconductor. No code was copied from existing DFU implementations.

### License Compatibility

All third-party licenses listed above are compatible with GPL-3.0-or-later:

- **Apache 2.0**: Compatible with GPL-3.0 per FSF guidance
- **MIT**: Compatible with GPL-3.0 (permissive)
- **BSD 3-Clause**: Compatible with GPL-3.0 (permissive)
- **PSF License**: Compatible with GPL-3.0 (permissive)
