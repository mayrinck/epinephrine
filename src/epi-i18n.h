#pragma once

#include <adwaita.h>

G_BEGIN_DECLS

/* Object data key: set on an AdwComboRow whose model is managed manually and
 * must not be touched by the generic translation pass (e.g. the language row). */
#define EPI_I18N_SKIP_MODEL "epi-tr-skip-model"

typedef struct
{
  char *code;        /* e.g. "en_US", "pt_BR" */
  char *native_name; /* native language name read from the file */
} EpiLanguage;

/* Sets the active language by code. lang_code may be NULL or "system" to
 * auto-detect from the system locale (English fallback). Unknown codes also
 * fall back to English. */
void epi_i18n_set_language (const char *lang_code);

/* Returns the currently requested code, or "system". Do not free. */
const char *epi_i18n_get_language (void);

/* Discovers every language available as lang/<code>.md from resources,
 * DATADIR and the current working directory. Returns a newly allocated array
 * of count entries; free it with epi_i18n_free_languages(). */
EpiLanguage *epi_i18n_list_languages (int *count);
void         epi_i18n_free_languages (EpiLanguage *langs, int count);

/* Returns the translation of key in the current language, falling back to
 * English and finally to the key itself. */
const char *epi_i18n_get (const char *key);

/* Walks the whole widget tree under root and translates every text-bearing
 * widget (and combo models) to the current language. Safe to call repeatedly
 * (e.g. on language switch). */
void epi_i18n_apply_widgets (GtkWidget *root);

/* TRUE while epi_i18n_apply_widgets() is running, so signal handlers triggered
 * by model/selection updates can avoid re-entrant retranslation. */
gboolean epi_i18n_in_apply (void);

G_END_DECLS
