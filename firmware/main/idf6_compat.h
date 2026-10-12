/* idf6_compat.h - IDF 6.x shim for the vendored Waveshare ST7789T driver
 * (firmware/main/Vernon_ST7789T, whitelisted vendor code - not editable).
 *
 * ESP-IDF 6.0 removed the deprecated `lcd_color_rgb_endian_t` type and the
 * LCD_RGB_ENDIAN_RGB/BGR constants from esp_lcd; the vendored driver still
 * names them in its public config struct and init routine. This file is
 * forced into every translation unit of the main component via
 * `-include` (main/CMakeLists.txt) so the vendor sources build unmodified.
 * The values keep the semantics the driver expects: RGB clears the MADCTL
 * BGR bit, BGR sets it (see esp_lcd_panel_commands.h LCD_CMD_BGR_BIT).
 */
#pragma once

#include "esp_lcd_types.h"

typedef enum {
    LCD_RGB_ENDIAN_RGB = 0,
    LCD_RGB_ENDIAN_BGR = 1,
} lcd_color_rgb_endian_t;