#pragma once

#include <adwaita.h>

G_BEGIN_DECLS

#define EPINEPHRINE_TYPE_APPLICATION (epinephrine_application_get_type ())
G_DECLARE_FINAL_TYPE (EpinephrineApplication, epinephrine_application, EPINEPHRINE, APPLICATION, AdwApplication)

EpinephrineApplication *epinephrine_application_new (void);

G_END_DECLS
