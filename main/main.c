#include "lcd.h"

static char *TAG = "main";

void app_main(void) {
    lcd_init(CS);
    //do sth here.
    lcd_delete(CS);
}
