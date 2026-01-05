# Changes Made to Pair with Philips Hue and Fix Colors

## 1. Fixed Syntax Error in `main/esp_zb_light.c`

There was a stray `};` on line 238 that prematurely closed the `esp_zb_task` function, leaving the rest of the Zigbee initialization code at global scope.

**Before:**
```c
        0x69, 0xCB, 0xF4, 0x2B, 0xC9, 0x3F, 0xEE, 0x31};
};
esp_zb_secur_TC_standard_distributed_key_set(secret_zll_trust_center_key);
```

**After:**
```c
        0xC8, 0xCB, 0xC5, 0x2E, 0x5D, 0x65, 0xD1, 0xB8};
    esp_zb_secur_TC_standard_distributed_key_set(secret_zll_trust_center_key);
```

## 2. Changed the ZLL Trust Center Link Key

The original key from [hanno's tweet](https://x.com/hanno/status/667996639890681857) (`9F 55 95 F1 02 57 C8 A4 69 CB F4 2B C9 3F EE 31`) is the **ZLL Master Key** used for touchlink commissioning.

However, Philips Hue uses **classical commissioning** (not touchlink), which requires a different key: the **ZLL Commissioning Trust Centre Link Key**.

**Before (ZLL Master Key - wrong for Hue):**
```c
0x9F, 0x55, 0x95, 0xF1, 0x02, 0x57, 0xC8, 0xA4,
0x69, 0xCB, 0xF4, 0x2B, 0xC9, 0x3F, 0xEE, 0x31
```

**After (ZLL Commissioning Trust Centre Link Key - correct for Hue):**
```c
0x81, 0x42, 0x86, 0x86, 0x5D, 0xC1, 0xC8, 0xB2,
0xC8, 0xCB, 0xC5, 0x2E, 0x5D, 0x65, 0xD1, 0xB8
```

## 3. Fixed RGB Color Order in `light_driver/src/light_driver.c`

The ESP32-C6-DevKitC has a WS2812 LED variant that uses RGB color order instead of the standard GRB order. Red and green were swapped.

**Changed in 5 locations (lines 37, 49, 59, 65, 73):**

**Before (GRB order):**
```c
led_strip_set_pixel(s_led_strip, 0, s_green * ratio, s_red * ratio, s_blue * ratio)
```

**After (RGB order):**
```c
led_strip_set_pixel(s_led_strip, 0, s_red * ratio, s_green * ratio, s_blue * ratio)
```

This fix ensures red displays as red and green displays as green in the Hue app.

## References

- [PeeVeeOne - Breakout Breakthrough](https://peeveeone.com/2016/11/breakout-breakthrough/) - Source of the correct ZLL Commissioning key
- [PeeVeeOne - Connecting Mesh Bee to Philips Hue](https://peeveeone.com/2016/05/connecting-mesh-bee-to-philips-hue/) - Background on Hue Zigbee commissioning
- [ESP32-C6-DevKitC-1 User Guide](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32c6/esp32-c6-devkitc-1/user_guide.html) - Hardware specifications
