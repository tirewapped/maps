# License

This repository, **maps** (offline RGB565 slippy-map viewer: LCD launcher
program + browser MapWindow), is released under the **Apache License,
Version 2.0**.

Full license text: <https://www.apache.org/licenses/LICENSE-2.0>

Copyright (c) 2026 by reticulous project contributors.

## Third-party software

### Vendored in this repository

None.

### Build-time dependencies

Declared in `esp-idf/idf_component.yml`:

| Component | Source | License |
|---|---|---|
| ESP-IDF (platform)   | espressif/esp-idf | Apache-2.0 |
| `espressif/esp_jpeg` (TJpgDec-based) | components.espressif.com | Apache-2.0 |

### Map data

Tile imagery rendered by this component is *not* shipped with the source.
Users supply their own tile sets; the tiles' map data is typically derived
from OpenStreetMap (© OpenStreetMap contributors, ODbL) — see your tile
source's terms and attribute appropriately in any product that ships them.
