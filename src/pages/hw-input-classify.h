#pragma once
#include <glib.h>
/* Suppress an auxiliary mouse interface only when the same USB device has
 * a matching boot-keyboard interface with the same kernel input name. */
gboolean hw_input_is_keyboard_aux_mouse (const char *sys_input_dir, const char *event_name);
