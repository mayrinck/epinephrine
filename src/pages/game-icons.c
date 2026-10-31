#include "page-dashboard.h"

static gboolean
game_icon_name_exists (const char *name)
{
  if (!name || !name[0]) return FALSE;

  GdkDisplay *display = gdk_display_get_default ();
  if (!display) return TRUE; /* no display yet: let GTK decide */

  GtkIconTheme *theme = gtk_icon_theme_get_for_display (display);
  if (!theme) return TRUE;

  return gtk_icon_theme_has_icon (theme, name);
}

GtkWidget *
create_game_icon_widget (const char *icon, int pixel_size)
{
  if (icon && (icon[0] == '/' || icon[0] == '~'))
    {
      char *path = (icon[0] == '~')
        ? g_build_filename (g_get_home_dir (), icon + 1, NULL)
        : g_strdup (icon);

      if (g_file_test (path, G_FILE_TEST_IS_REGULAR))
        {
          /* Static image (PNG, JPEG, GIF, WebP, ICO, BMP, TIFF, SVG).
             Always scaled to fit pixel_size x pixel_size preserving the
             aspect ratio — up or down — so every icon takes the same area
             no matter how large its source artwork is. GTK 4's GtkImage has
             no animation support, so the first frame is all we can show. */
          GError *err = NULL;
          GdkPixbuf *pixbuf = gdk_pixbuf_new_from_file_at_scale (path, 256, 256, TRUE, &err);
          if (pixbuf)
            {
              int w = gdk_pixbuf_get_width (pixbuf), h = gdk_pixbuf_get_height (pixbuf);
              int left = 0, top = 0, right = w - 1, bottom = h - 1;
              if (gdk_pixbuf_get_has_alpha (pixbuf))
                {
                  left = w; top = h; right = bottom = -1;
                  const guchar *pixels = gdk_pixbuf_read_pixels (pixbuf);
                  int stride = gdk_pixbuf_get_rowstride (pixbuf);
                  int channels = gdk_pixbuf_get_n_channels (pixbuf);
                  for (int y = 0; y < h; y++)
                    for (int x = 0; x < w; x++)
                      if (pixels[y * stride + x * channels + channels - 1] > 8)
                        { left = MIN (left, x); right = MAX (right, x); top = MIN (top, y); bottom = MAX (bottom, y); }
                }
              if (right >= left && bottom >= top)
                {
                  GdkPixbuf *crop = gdk_pixbuf_new_subpixbuf (pixbuf, left, top, right-left+1, bottom-top+1);
                  double scale = (double) pixel_size / MAX (right-left+1, bottom-top+1);
                  GdkPixbuf *scaled = gdk_pixbuf_scale_simple (crop,
                    MAX (1, (int) ((right-left+1)*scale)), MAX (1, (int) ((bottom-top+1)*scale)), GDK_INTERP_BILINEAR);
                  GdkTexture *texture = gdk_texture_new_for_pixbuf (scaled);
                  GtkWidget *img = gtk_picture_new_for_paintable (GDK_PAINTABLE (texture));
                  gtk_picture_set_content_fit (GTK_PICTURE (img), GTK_CONTENT_FIT_CONTAIN);
                  gtk_widget_set_size_request (img, pixel_size, pixel_size);
                  gtk_widget_set_halign (img, GTK_ALIGN_CENTER);
                  gtk_widget_set_valign (img, GTK_ALIGN_CENTER);
                  g_object_unref (texture); g_object_unref (scaled); g_object_unref (crop);
                  g_object_unref (pixbuf); g_free (path);
                  return img;
                }
              g_object_unref (pixbuf);
            }
          if (err) g_error_free (err);
        }

      g_free (path);
      /* Unreadable file: fall through to the themed-icon path below. */
    }

  const char *name = (icon && icon[0]) ? icon : "input-gaming-symbolic";
  if (!game_icon_name_exists (name))
    name = "input-gaming-symbolic";

  GtkWidget *img = gtk_image_new_from_icon_name (name);
  gtk_image_set_pixel_size (GTK_IMAGE (img), pixel_size);
  gtk_widget_set_size_request (img, pixel_size, pixel_size);
  return img;
}
