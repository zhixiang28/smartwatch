/* Included by LVGL (CONFIG_LV_ASSERT_HANDLER_INCLUDE). LVGL's default assert handler is
 * `while(1);`, which froze the screen forever with no trace. Abort instead: the panic
 * handler reboots the watch and the crash is saved as a core dump the phone can fetch. */
#pragma once
#include <stdlib.h>
#undef LV_ASSERT_HANDLER
#define LV_ASSERT_HANDLER abort();
