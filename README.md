# ESP32 Lightbulb example that works with Hue bridge

This is Espressif's [HA_color_dimmable_light](https://github.com/espressif/esp-zigbee-sdk/tree/a943a9118e9ad110e2641e1187fd4c5d533f8a06/examples/esp_zigbee_HA_sample/HA_color_dimmable_light)
adjusted to successfully link against Hue bridge, and function
like a properly behaved light (mostly).

Mind you, you'll need to adjust the trust center link key
(heed the `FIXME` in `main/esp_zb_light.c`).

Details in my [Zigbee: Hue-llo world!](https://wejn.org/2025/01/zigbee-hue-llo-world/)
blog post.

And if you want a firmware that's a bit more full-featured,
then check out [Introducing e32: firmware for esp32-c6 based White Ambiance
light](https://wejn.org/2025/03/introducing-e32wamb-firmware-for-esp32-c6-based-white-ambiance/)
post and the associated [wejn/e32wamb](https://github.com/wejn/e32wamb/) repo.

To compile:

``` sh
./in-docker.sh idf.py set-target esp32-c6 build
```

To flash:

``` sh
# pipx install esptool
cd build
esptool.py --chip esp32c6 -b 460800 \
  --before default_reset --after hard_reset \
  write_flash "@flash_args"
```

## Flashing with ESP-IDF

If using ESP-IDF directly (e.g., ESP-IDF 6.0):

### Environment Setup

Before running any `idf.py` commands, activate the ESP-IDF environment:

```bash
source $HOME/esp/v6.0/esp-idf/export.sh
```

### Regular Flash (Preserves Zigbee Pairing)

```bash
idf.py -p /dev/cu.usbmodem1101 flash
```

Regular flashing only updates the application code and **preserves the NVS partition** where Zigbee network credentials are stored. Your device will automatically reconnect to the Hue bridge without needing to re-pair.

### Full Factory Reset (Erases Zigbee Pairing)

```bash
idf.py -p /dev/cu.usbmodem1101 erase-flash
```

This erases **everything** including the Zigbee pairing data. Use this if:
- The device is stuck in a loop
- You need to pair with a different Hue bridge
- You want to start completely fresh

After `erase-flash`, you'll need to re-pair the device with your Hue bridge.

### Build, Flash, and Monitor

```bash
idf.py -p /dev/cu.usbmodem1101 flash monitor
```

Note: Monitor requires a TTY. If running from a script, run monitor separately in another terminal:

```bash
idf.py -p /dev/cu.usbmodem1101 monitor
```

## Changing Device Type (RGB vs Color Temperature vs Extended Color)

### Current Configuration
The upstream Espressif sample this project started from is a **Color Dimmable Light** (`ESP_ZB_HA_COLOR_DIMMABLE_LIGHT_DEVICE_ID`) which supports:
- RGB color control (XY color space)
- Hue/Saturation
- Brightness/Level

### Other Device Types

**Color Temperature Light** (`ESP_ZB_HA_COLOR_TEMPERATURE_LIGHT_DEVICE_ID`):
- Warm/cold white control
- No RGB colors
- Adjustable color temperature (in mireds)

**Extended Color Light** (`ESP_ZB_HA_EXTENDED_COLOR_LIGHT_DEVICE_ID`):
- **Both** RGB color control AND color temperature
- Supports all features of both types above
- Best for full-featured lights

### Switching Device Types

**Important:** Changing the device type requires re-pairing with the Hue bridge.

When you pair a Zigbee device, it announces its capabilities to the bridge. The bridge stores this information. If you change the device type, the bridge will have incorrect expectations.

**Steps to change device type:**

1. Delete the light from Hue app
2. Update `main/esp_zb_light.c`:
   - Change `ESP_ZB_HA_COLOR_DIMMABLE_LIGHT_DEVICE_ID` to desired type
   - Update the configuration struct (e.g., `esp_zb_color_temperature_light_cfg_t`)
   - Add/remove appropriate clusters
3. Build and flash the new firmware
4. Run `idf.py -p /dev/cu.usbmodem1101 erase-flash` to clear old credentials
5. Flash again and re-pair with Hue bridge

The bridge will now recognize the device with its new capabilities.

### Current Device Type: Color Temperature + Dimming

This firmware currently identifies as a **Color Temperature Light** (`ESP_ZB_HA_COLOR_TEMPERATURE_LIGHT_DEVICE_ID`).
It supports:
- Warm/cold white control (mireds)
- Dimming (level)
No RGB color control is exposed to Hue.

## Additional Runtime Features Implemented

The current codebase includes several behaviors not covered above.

### 1. Local Button Control

Button input is enabled on `GPIO1` (`main/esp_zb_light.h`):
- Short press: toggle on/off
- Long press (`600ms`): start continuous dimming
- While held: level steps by `10` every `200ms`
- Dimming direction auto-reverses at `0` and `255`
- Debounce: `50ms`

### 2. Persisted Light State (Power / Level / Color Temperature)

The firmware persists state in NVS namespace `light`:
- `power` (`u8`)
- `level` (`u8`)
- `ct` (`u16`)

Persistence is delayed and coalesced (write after `10s` of inactivity) to reduce flash wear.
On boot, values are restored and applied during deferred driver initialization.

### 3. Center-Based Strip Rendering

The strip is rendered as a centered lit segment:
- Active LED count = `CONFIG_EXAMPLE_STRIP_LED_NUMBER * level / 255`
- Lit region grows/shrinks from the middle outward/inward

Current defaults in `light_driver/include/light_driver.h`:
- `CONFIG_EXAMPLE_STRIP_LED_GPIO = 4`
- `CONFIG_EXAMPLE_STRIP_LED_NUMBER = 144`

### 4. 1-Second Fade Transitions (On and Off)

The light driver applies 1-second fades using 20 frames:
- Fade-in on effective visible transition `0 -> >0`
- Fade-out on effective visible transition `>0 -> 0`

This works with Hue command ordering (for example `level=0`, then `on`, then `level>0`) because trigger logic is based on visible lit-count transition, not only On/Off attribute order.

To prevent brief animation glitches, the periodic refresh callback is suspended while a fade is running.

### 5. Periodic Diagnostics

The driver logs estimated LED current every `5s`:
- Logs `0.0 mA` when off
- Logs an estimate based on lit count, level, and RGB components when on

To clean up:

``` sh
./in-docker.sh rm -rf build dependencies.lock managed_components/ sdkconfig
```

To change your board's MAC (or other ZB parameters):

``` sh
./in-docker.sh python3 esp_zb_mfg_tool.py \
  --manufacturer_name Espressif --manufacturer_code 0x131B \
  --channel_mask 0x07FFF800 \
  --mac_address CAFEBEEF50C0FFA0
esptool.py write_flash 0x1d8000 ./bin/CAFEBEEF50C0FFA0.bin
```

Note: `0x1d8000` is the location of your `zb_fct` in `partitions.csv`.
And the other possible parameters (for `esp_zb_mfg_tool`) can be figured
out easily from its source.
