#include "application.h"

int
main (int argc, char *argv[])
{
  EpinephrineApplication *app = epinephrine_application_new ();

  int status = g_application_run (G_APPLICATION (app), argc, argv);

  g_object_unref (app);

  return status;
}
