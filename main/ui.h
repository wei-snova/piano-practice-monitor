/* 480x480, four-page touchscreen UI: Piano, Cello, Diagnostics, Goals. */
#pragma once

#include "app.h"

/* Call once with the display lock held. Swipe to navigate; active practice
 * follows the detected instrument except while Diagnostics is open. */
void ui_init(float sens_db);
