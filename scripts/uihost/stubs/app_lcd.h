#ifndef HOST_APP_LCD_H
#define HOST_APP_LCD_H
#include <stdint.h>
#define EXAMPLE_LCD_H_RES 1024
#define EXAMPLE_LCD_V_RES 600
int app_lcd_get_fb(int idx, void **fb);
int app_lcd_flush(int idx);
#endif
