#pragma once

#include <adwaita.h>

/* Language index as saved in settings (0 = default, 1 = pt_BR, 2 = en_US). */
void epi_i18n_set_language (int lang_index);

/* Loads/keeps the table for the current language; returns translation or the
 * source key itself when no translation exists. */
const char *epi_i18n_get (const char *key);

/* Walks the whole widget tree under root and translates every text-bearing
 * widget to the current language. Safe to call repeatedly (e.g. on language
 * switch). */
void epi_i18n_apply_widgets (GtkWidget *root);
