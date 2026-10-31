#include <adwaita.h>
#include <glib/gstdio.h>
#include "pages/page-dashboard.h"
int main(void) {
  if (!gtk_init_check ()) return 77;
  g_autofree char *dir = g_dir_make_tmp ("epi-icon-test-XXXXXX", NULL);
  g_autofree char *path = g_build_filename (dir, "padded.png", NULL);
  GdkPixbuf *pix = gdk_pixbuf_new (GDK_COLORSPACE_RGB, TRUE, 8, 64, 64);
  gdk_pixbuf_fill (pix, 0);
  guchar *pixels = gdk_pixbuf_get_pixels (pix);
  for (int y=24;y<40;y++) for(int x=24;x<40;x++) {
    guchar *p=pixels+y*gdk_pixbuf_get_rowstride(pix)+x*4;
    p[0]=255;p[3]=255;
  }
  g_assert_true (gdk_pixbuf_save (pix, path, "png", NULL, NULL));
  GtkWidget *icon = create_game_icon_widget (path, 28);
  g_object_ref_sink (icon);
  g_assert_true (GTK_IS_PICTURE (icon));
  GdkPaintable *paintable = gtk_picture_get_paintable (GTK_PICTURE (icon));
  g_assert_cmpint (gdk_paintable_get_intrinsic_width (paintable), ==, 28);
  g_assert_cmpint (gdk_paintable_get_intrinsic_height (paintable), ==, 28);
  g_object_unref(icon);g_object_unref(pix);g_remove(path);g_rmdir(dir);
  return 0;
}
