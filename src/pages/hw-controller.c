#define _GNU_SOURCE
#include "page-hardware.h"
#include "hw-controller.h"
#include "hw-gpu-detail.h"
#include "page-dashboard.h"
#include "usb-ids.h"
#include "../epi-i18n.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <glib-unix.h>

#define tr(x) epi_i18n_get (x)

/* ── Controller Name Database (USB VID:PID → proper name) ── */

typedef struct {
    uint16_t vendor;
    uint16_t product;
    const char *name;
    const char *url;
    const char *category;
} ControllerEntry;

/* Runtime entries from JOY_INDEX.md */
typedef struct {
    char *name;
    char *url;
    char *category;
} ControllerIndexEntry;

static void
free_controller_index_entry (gpointer data)
{
  ControllerIndexEntry *e = data;
  if (e)
    {
      g_free (e->name);
      g_free (e->url);
      g_free (e->category);
      g_free (e);
    }
}

static const ControllerEntry controller_db[] = {
  { 0x0079, 0x187c, "KROM KUMITE",
    "https://www.kromgaming.com/pt/comandos/kumite", "Arcade" },
};
static const int controller_db_count = G_N_ELEMENTS (controller_db);

const char *
hw_controller_lookup_name (HardwarePage *page, uint16_t vendor, uint16_t product)
{
  /* Try online/usb.ids database first */
  if (page && page->usb_ids)
    {
      const char *name = usb_ids_lookup (page->usb_ids, vendor, product);
      if (name) return name;
    }
  /* Fall back to JOY_INDEX.md */
  if (page && page->controller_index)
    {
      char key[16];
      g_snprintf (key, sizeof (key), "%04x:%04x", vendor, product);
      ControllerIndexEntry *e = g_hash_table_lookup (page->controller_index, key);
      if (e && e->name) return e->name;
    }
  /* Fall back to hardcoded database */
  for (int i = 0; i < controller_db_count; i++)
    if (controller_db[i].vendor == vendor && controller_db[i].product == product)
      return controller_db[i].name;
  return NULL;
}

const char *
hw_controller_lookup_url (HardwarePage *page, uint16_t vendor, uint16_t product)
{
  if (page && page->controller_index)
    {
      char key[16];
      g_snprintf (key, sizeof (key), "%04x:%04x", vendor, product);
      ControllerIndexEntry *e = g_hash_table_lookup (page->controller_index, key);
      if (e && e->url) return e->url;
    }
  for (int i = 0; i < controller_db_count; i++)
    if (controller_db[i].vendor == vendor && controller_db[i].product == product)
      return controller_db[i].url;
  return NULL;
}

const char *
hw_controller_lookup_category (HardwarePage *page, uint16_t vendor, uint16_t product)
{
  if (page && page->controller_index)
    {
      char key[16];
      g_snprintf (key, sizeof (key), "%04x:%04x", vendor, product);
      ControllerIndexEntry *e = g_hash_table_lookup (page->controller_index, key);
      if (e && e->category) return e->category;
    }
  for (int i = 0; i < controller_db_count; i++)
    if (controller_db[i].vendor == vendor && controller_db[i].product == product)
      return controller_db[i].category;
  return NULL;
}

/* Load controller database from JOY_INDEX.md (GResource → DATADIR → CWD) */
void
hw_controller_load_index (HardwarePage *page)
{
  if (page->controller_index) return;

  page->controller_index = g_hash_table_new_full (g_str_hash, g_str_equal,
    g_free, free_controller_index_entry);

  char *content = NULL;
  GBytes *gbytes = g_resources_lookup_data (
    "/io/github/mayrinck/epinephrine/database/JOY_INDEX.md", 0, NULL);
  if (gbytes)
    {
      content = g_strndup (g_bytes_get_data (gbytes, NULL),
        g_bytes_get_size (gbytes));
      g_bytes_unref (gbytes);
    }
  if (!content)
    g_file_get_contents (DATADIR "/epinephrine/database/JOY_INDEX.md", &content, NULL, NULL);
  if (!content)
    {
      char *cwd = g_get_current_dir ();
      char *rel = g_build_filename (cwd, "database", "JOY_INDEX.md", NULL);
      g_file_get_contents (rel, &content, NULL, NULL);
      g_free (cwd);
      g_free (rel);
    }

  if (!content) return;

  gboolean in_table = FALSE;
  char **lines = g_strsplit (content, "\n", -1);
  g_free (content);

  for (int i = 0; lines[i]; i++)
    {
      const char *line = lines[i];

      if (g_str_has_prefix (line, "|---|---"))
        {
          in_table = TRUE;
          continue;
        }

      if (!in_table || line[0] != '|') continue;

      char **cols = g_strsplit (line, "|", 0);
      if (!cols[1])
        {
          g_strfreev (cols);
          continue;
        }

      /* Columns: Name | VID:PID | Category | URL | Features | Connection | Buttons */
      char *name = g_strstrip (cols[1]);
      char *vidpid_str = cols[2] ? g_strstrip (cols[2]) : NULL;
      char *cat_str = cols[3] ? g_strstrip (cols[3]) : NULL;
      char *url_str = cols[4] ? g_strstrip (cols[4]) : NULL;

      if (!name || !name[0] || !vidpid_str || !vidpid_str[0])
        {
          g_strfreev (cols);
          continue;
        }

      /* Parse VID:PID format "XXXX:XXXX" */
      unsigned int v = 0, p = 0;
      if (sscanf (vidpid_str, "%04x:%04x", &v, &p) != 2 || v == 0)
        {
          g_strfreev (cols);
          continue;
        }

      char key[16];
      g_snprintf (key, sizeof (key), "%04x:%04x", v, p);

      ControllerIndexEntry *entry = g_new0 (ControllerIndexEntry, 1);
      entry->name = g_strdup (name);
      entry->url = url_str && url_str[0] ? g_strdup (url_str) : NULL;
      entry->category = cat_str && cat_str[0] ? g_strdup (cat_str) : NULL;

      g_hash_table_insert (page->controller_index, g_strdup (key), entry);

      g_strfreev (cols);
    }

  g_strfreev (lines);
}

/* ---- Controller Event Watch ---- */

static void
calibrate_axis (int fd, int axis, int *out_min, int *out_max)
{
  struct input_absinfo absinfo;
  if (ioctl (fd, EVIOCGABS (axis), &absinfo) >= 0)
    {
      *out_min = absinfo.minimum;
      *out_max = absinfo.maximum;
    }
  else
    {
      *out_min = 0;
      *out_max = 255;
    }
}

gboolean
hw_controller_event (gint fd, GIOCondition condition, gpointer user_data)
{
  (void) condition;
  HardwarePage *pg = user_data;
  struct input_event ev;
  ssize_t n;

  while ((n = read (fd, &ev, sizeof (ev))) == sizeof (ev))
    {
      if (ev.type == EV_KEY && ev.code <= KEY_MAX)
        {
          pg->ctrl_state->buttons[ev.code] = ev.value;
          int idx = -1;
          switch (ev.code)
            {
              /* Face buttons */
              case BTN_A:          idx = IND_A; break;
              case BTN_B:          idx = IND_B; break;
              case BTN_X:          idx = IND_X; break;
              case BTN_Y:          idx = IND_Y; break;
              /* Alternate face button mappings (some controllers use these) */
              case BTN_C:          idx = IND_X; break;
              case BTN_Z:          idx = IND_Y; break;
              case BTN_TRIGGER:    idx = IND_A; break;
              case BTN_THUMB:      idx = IND_B; break;
              case BTN_THUMB2:     idx = IND_X; break;
              case BTN_TOP:        idx = IND_Y; break;
              /* Bumpers / triggers via KEY codes */
              case BTN_TOP2:       idx = IND_L1; break;
              case BTN_PINKIE:     idx = IND_R1; break;
              case BTN_BASE:       idx = IND_L2; break;
              case BTN_BASE2:      idx = IND_R2; break;
              case BTN_BASE3:      idx = IND_SELECT; break;
              case BTN_BASE4:      idx = IND_START; break;
              case BTN_BASE5:      idx = IND_L3; break;
              case BTN_BASE6:      idx = IND_R3; break;
              case BTN_DEAD:       idx = IND_SELECT; break;
              /* D-pad */
              case BTN_DPAD_UP:    idx = IND_DPAD_UP; break;
              case BTN_DPAD_DOWN:  idx = IND_DPAD_DOWN; break;
              case BTN_DPAD_LEFT:  idx = IND_DPAD_LEFT; break;
              case BTN_DPAD_RIGHT: idx = IND_DPAD_RIGHT; break;
              /* Standard button names */
              case BTN_START:      idx = IND_START; break;
              case BTN_SELECT:     idx = IND_SELECT; break;
              case BTN_TL:         idx = IND_L1; break;
              case BTN_TR:         idx = IND_R1; break;
              case BTN_TL2:        idx = IND_L2; break;
              case BTN_TR2:        idx = IND_R2; break;
              /* Thumbstick clicks */
              case BTN_THUMBL:
                {
                  pg->ctrl_state->thumb_l_pressed = ev.value;
                  if (pg->analog_l_canvas)
                    gtk_widget_queue_draw (pg->analog_l_canvas);
                  idx = IND_L3;
                  break;
                }
              case BTN_THUMBR:
                {
                  pg->ctrl_state->thumb_r_pressed = ev.value;
                  if (pg->analog_r_canvas)
                    gtk_widget_queue_draw (pg->analog_r_canvas);
                  idx = IND_R3;
                  break;
                }
              case BTN_MODE:       idx = IND_GUIDE; break;
              /* Grip/paddle buttons (mapped to available indicators) */
              case BTN_GRIPL:      idx = IND_L1; break;
              case BTN_GRIPR:      idx = IND_R1; break;
              case BTN_GRIPL2:     idx = IND_L2; break;
              case BTN_GRIPR2:     idx = IND_R2; break;
              /* Extra / macro buttons (M1–M4 on many controllers) */
              case BTN_TRIGGER_HAPPY1: idx = IND_M1; break;
              case BTN_TRIGGER_HAPPY2: idx = IND_M2; break;
              case BTN_TRIGGER_HAPPY3: idx = IND_M3; break;
              case BTN_TRIGGER_HAPPY4: idx = IND_M4; break;
              /* D-pad via KEY_* codes (some controllers report these) */
              case KEY_UP:          idx = IND_DPAD_UP; break;
              case KEY_DOWN:        idx = IND_DPAD_DOWN; break;
              case KEY_LEFT:        idx = IND_DPAD_LEFT; break;
              case KEY_RIGHT:       idx = IND_DPAD_RIGHT; break;
              /* Map unknown BTN_ codes (0x120-0x2ff) to a generic indicator
                 so no input is silently dropped */
              default:
                if (ev.code >= BTN_JOYSTICK && ev.code < KEY_MAX)
                  {
                    /* Cycle through indicators for unmapped buttons */
                    static const int fallback_map[] = {
                      IND_A, IND_B, IND_X, IND_Y,
                      IND_L1, IND_R1, IND_L2, IND_R2,
                      IND_SELECT, IND_START, IND_GUIDE,
                      IND_L3, IND_R3
                    };
                    int map_idx = (ev.code - BTN_JOYSTICK) % G_N_ELEMENTS (fallback_map);
                    idx = fallback_map[map_idx];
                  }
                break;
            }
          if (idx >= 0 && pg->indicators[idx])
            {
              if (ev.value)
                gtk_widget_add_css_class (pg->indicators[idx], "ctrl-indicator-active");
              else
                gtk_widget_remove_css_class (pg->indicators[idx], "ctrl-indicator-active");
            }
        }
      else if (ev.type == EV_ABS)
        {
          switch (ev.code)
            {
              case ABS_X:  pg->ctrl_state->lx = ev.value; break;
              case ABS_Y:  pg->ctrl_state->ly = ev.value; break;
              case ABS_RX: pg->ctrl_state->rx = ev.value; break;
              case ABS_RY: pg->ctrl_state->ry = ev.value; break;
              /* D-pad as HAT axes (many controllers) */
              case ABS_HAT0X:
                {
                  int old_l = pg->ctrl_state->buttons[ABS_HAT0X];
                  int new_l = ev.value == -1;
                  int old_r = old_l == 1;
                  int new_r = ev.value == 1;
                  pg->ctrl_state->buttons[ABS_HAT0X] = ev.value;
                  if (old_l != new_l)
                    {
                      if (new_l) gtk_widget_add_css_class (pg->indicators[IND_DPAD_LEFT], "ctrl-indicator-active");
                      else gtk_widget_remove_css_class (pg->indicators[IND_DPAD_LEFT], "ctrl-indicator-active");
                    }
                  if (old_r != new_r)
                    {
                      if (new_r) gtk_widget_add_css_class (pg->indicators[IND_DPAD_RIGHT], "ctrl-indicator-active");
                      else gtk_widget_remove_css_class (pg->indicators[IND_DPAD_RIGHT], "ctrl-indicator-active");
                    }
                  break;
                }
              case ABS_HAT0Y:
                {
                  int old_u = pg->ctrl_state->buttons[ABS_HAT0Y];
                  int new_u = ev.value == -1;
                  int old_d = old_u == 1;
                  int new_d = ev.value == 1;
                  pg->ctrl_state->buttons[ABS_HAT0Y] = ev.value;
                  if (old_u != new_u)
                    {
                      if (new_u) gtk_widget_add_css_class (pg->indicators[IND_DPAD_UP], "ctrl-indicator-active");
                      else gtk_widget_remove_css_class (pg->indicators[IND_DPAD_UP], "ctrl-indicator-active");
                    }
                  if (old_d != new_d)
                    {
                      if (new_d) gtk_widget_add_css_class (pg->indicators[IND_DPAD_DOWN], "ctrl-indicator-active");
                      else gtk_widget_remove_css_class (pg->indicators[IND_DPAD_DOWN], "ctrl-indicator-active");
                    }
                  break;
                }
              /* Analog triggers: use dynamic threshold based on axis range */
              case ABS_Z:
              case ABS_BRAKE:
                {
                  static int z_min = 0, z_max = 255;
                  static gboolean z_calibrated = FALSE;
                  if (!z_calibrated) calibrate_axis (fd, ABS_Z, &z_min, &z_max);
                  int threshold = z_min + (z_max - z_min) / 10;
                  int pressed = ev.value > threshold;
                  int old = pg->ctrl_state->buttons[ev.code];
                  pg->ctrl_state->buttons[ev.code] = pressed;
                  if (old != pressed)
                    {
                      if (pressed) gtk_widget_add_css_class (pg->indicators[IND_L2], "ctrl-indicator-active");
                      else gtk_widget_remove_css_class (pg->indicators[IND_L2], "ctrl-indicator-active");
                    }
                  break;
                }
              case ABS_RZ:
              case ABS_GAS:
                {
                  static int rz_min = 0, rz_max = 255;
                  static gboolean rz_calibrated = FALSE;
                  if (!rz_calibrated) calibrate_axis (fd, ABS_RZ, &rz_min, &rz_max);
                  int threshold = rz_min + (rz_max - rz_min) / 10;
                  int pressed = ev.value > threshold;
                  int old = pg->ctrl_state->buttons[ev.code];
                  pg->ctrl_state->buttons[ev.code] = pressed;
                  if (old != pressed)
                    {
                      if (pressed) gtk_widget_add_css_class (pg->indicators[IND_R2], "ctrl-indicator-active");
                      else gtk_widget_remove_css_class (pg->indicators[IND_R2], "ctrl-indicator-active");
                    }
                  break;
                }
            }
          char stick[64];
          g_snprintf (stick, sizeof (stick), "LX: %-4d  LY: %-4d",
            pg->ctrl_state->lx, pg->ctrl_state->ly);
          gtk_label_set_text (GTK_LABEL (pg->axis_lx), stick);
          g_snprintf (stick, sizeof (stick), "RX: %-4d  RY: %-4d",
            pg->ctrl_state->rx, pg->ctrl_state->ry);
          gtk_label_set_text (GTK_LABEL (pg->axis_rx), stick);
          if (pg->analog_l_canvas)
            gtk_widget_queue_draw (pg->analog_l_canvas);
          if (pg->analog_r_canvas)
            gtk_widget_queue_draw (pg->analog_r_canvas);
        }
    }

  /* Device disconnected or error: clean up and stop watching */
  if (n == 0 || (n < 0 && (errno == ENODEV || errno == ENXIO || errno == EIO)))
    {
      hw_controller_close (pg);
      return G_SOURCE_REMOVE;
    }

  return G_SOURCE_CONTINUE;
}

/* ---- Controller UI ---- */

#define BASE_IND_SZ 40

static const char *indicator_labels_xbox[IND_COUNT] = {
  [IND_A]        = "A",
  [IND_B]        = "B",
  [IND_X]        = "X",
  [IND_Y]        = "Y",
  [IND_DPAD_UP]    = "\xe2\x86\x91",      /* ↑ */
  [IND_DPAD_DOWN]  = "\xe2\x86\x93",      /* ↓ */
  [IND_DPAD_LEFT]  = "\xe2\x86\x90",      /* ← */
  [IND_DPAD_RIGHT] = "\xe2\x86\x92",      /* → */
  [IND_START]      = "St",
  [IND_SELECT]     = "Se",
  [IND_L1]        = "LB",
  [IND_R1]        = "RB",
  [IND_L2]        = "LT",
  [IND_R2]        = "RT",
  [IND_GUIDE]     = "Ho",
  [IND_L3]        = "LS",
  [IND_R3]        = "RS",
  [IND_M1]        = "M1",
  [IND_M2]        = "M2",
  [IND_M3]        = "M3",
  [IND_M4]        = "M4",
};

static const char *indicator_labels_ps[IND_COUNT] = {
  [IND_A]        = "\xc3\x97",            /* × */
  [IND_B]        = "\xe2\x97\x8b",        /* ○ */
  [IND_X]        = "\xe2\x96\xa1",        /* □ */
  [IND_Y]        = "\xe2\x96\xb3",        /* △ */
  [IND_DPAD_UP]    = "\xe2\x86\x91",      /* ↑ */
  [IND_DPAD_DOWN]  = "\xe2\x86\x93",      /* ↓ */
  [IND_DPAD_LEFT]  = "\xe2\x86\x90",      /* ← */
  [IND_DPAD_RIGHT] = "\xe2\x86\x92",      /* → */
  [IND_START]      = "St",
  [IND_SELECT]     = "Se",
  [IND_L1]        = "L1",
  [IND_R1]        = "R1",
  [IND_L2]        = "L2",
  [IND_R2]        = "R2",
  [IND_GUIDE]     = "Ho",
  [IND_L3]        = "L3",
  [IND_R3]        = "R3",
  [IND_M1]        = "M1",
  [IND_M2]        = "M2",
  [IND_M3]        = "M3",
  [IND_M4]        = "M4",
};

static const char *indicator_labels_generic[IND_COUNT] = {
  [IND_A]        = "B1",
  [IND_B]        = "B2",
  [IND_X]        = "B3",
  [IND_Y]        = "B4",
  [IND_DPAD_UP]    = "\xe2\x86\x91",
  [IND_DPAD_DOWN]  = "\xe2\x86\x93",
  [IND_DPAD_LEFT]  = "\xe2\x86\x90",
  [IND_DPAD_RIGHT] = "\xe2\x86\x92",
  [IND_START]      = "St",
  [IND_SELECT]     = "Se",
  [IND_L1]        = "L1",
  [IND_R1]        = "R1",
  [IND_L2]        = "L2",
  [IND_R2]        = "R2",
  [IND_GUIDE]     = "Ho",
  [IND_L3]        = "L3",
  [IND_R3]        = "R3",
  [IND_M1]        = "M1",
  [IND_M2]        = "M2",
  [IND_M3]        = "M3",
  [IND_M4]        = "M4",
};

static const char **indicator_labels = indicator_labels_xbox;

static GtkWidget *
make_indicator (HardwarePage *page, int idx)
{
  GtkWidget *dot = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_size_request (dot, BASE_IND_SZ, BASE_IND_SZ);
  gtk_widget_add_css_class (dot, "ctrl-indicator");
  gtk_widget_set_halign (dot, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (dot, GTK_ALIGN_CENTER);

  if (indicator_labels[idx])
    {
      GtkWidget *lbl = gtk_label_new (indicator_labels[idx]);
      gtk_widget_add_css_class (lbl, "ctrl-label");
      gtk_widget_set_halign (lbl, GTK_ALIGN_CENTER);
      gtk_widget_set_valign (lbl, GTK_ALIGN_CENTER);
      gtk_widget_set_hexpand (lbl, TRUE);
      gtk_box_append (GTK_BOX (dot), lbl);
    }

  page->indicators[idx] = dot;
  return dot;
}

static void
update_indicator_labels (HardwarePage *page, const char **labels)
{
  indicator_labels = labels;
  for (int i = 0; i < IND_COUNT; i++)
    {
      GtkWidget *ind = page->indicators[i];
      if (!ind || !labels[i]) continue;
      GtkWidget *child = gtk_widget_get_first_child (ind);
      if (child && GTK_IS_LABEL (child))
        gtk_label_set_text (GTK_LABEL (child), labels[i]);
    }
}

enum { STICK_LEFT, STICK_RIGHT };

static void
on_stick_draw (GtkDrawingArea *area, cairo_t *cr, int w, int h, gpointer user_data)
{
  (void) area;
  HardwarePage *page = user_data;
  if (!page->ctrl_state) return;

  int stick = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (area), "stick-id"));
  int sx = (stick == STICK_RIGHT) ? page->ctrl_state->rx : page->ctrl_state->lx;
  int sy = (stick == STICK_RIGHT) ? page->ctrl_state->ry : page->ctrl_state->ly;
  gboolean thumb_pressed = (stick == STICK_RIGHT)
    ? page->ctrl_state->thumb_r_pressed
    : page->ctrl_state->thumb_l_pressed;

  double cx = w / 2.0;
  double cy = h / 2.0;
  double radius = MIN (cx, cy) - 4;

  /* Background — accent color when thumbstick pressed */
  if (thumb_pressed)
    cairo_set_source_rgba (cr, 0.3, 0.6, 1.0, 0.35);
  else
    cairo_set_source_rgba (cr, 0.2, 0.2, 0.2, 0.6);
  cairo_arc (cr, cx, cy, radius, 0, 2 * G_PI);
  cairo_fill (cr);

  /* Border */
  if (thumb_pressed)
    cairo_set_source_rgba (cr, 0.3, 0.6, 1.0, 0.8);
  else
    cairo_set_source_rgba (cr, 0.5, 0.5, 0.5, 0.8);
  cairo_set_line_width (cr, 1.5);
  cairo_arc (cr, cx, cy, radius, 0, 2 * G_PI);
  cairo_stroke (cr);

  /* Crosshair */
  cairo_set_source_rgba (cr, 0.6, 0.6, 0.6, 0.4);
  cairo_set_line_width (cr, 0.5);
  cairo_move_to (cr, cx - radius, cy);
  cairo_line_to (cr, cx + radius, cy);
  cairo_move_to (cr, cx, cy - radius);
  cairo_line_to (cr, cx, cy + radius);
  cairo_stroke (cr);

  /* Stick position dot */
  double nx = sx / 32767.0;
  double ny = sy / 32767.0;
  double dx = cx + nx * (radius - 6);
  double dy = cy + ny * (radius - 6);

  cairo_set_source_rgba (cr, 0.3, 0.7, 1.0, 0.9);
  cairo_arc (cr, dx, dy, 6, 0, 2 * G_PI);
  cairo_fill (cr);
}

static GtkWidget *
build_controller_test_layout_standard (HardwarePage *page)
{
  GtkWidget *test_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_set_margin_start (test_box, 16);
  gtk_widget_set_margin_end (test_box, 16);
  gtk_widget_set_margin_top (test_box, 8);
  gtk_widget_set_margin_bottom (test_box, 8);

  /* Top row: LT  L1  ─  R1  RT */
  GtkWidget *top_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 10);
  gtk_widget_set_halign (top_row, GTK_ALIGN_CENTER);

  page->indicators[IND_L2] = make_indicator (page, IND_L2);
  gtk_box_append (GTK_BOX (top_row), page->indicators[IND_L2]);

  page->indicators[IND_L1] = make_indicator (page, IND_L1);
  gtk_box_append (GTK_BOX (top_row), page->indicators[IND_L1]);

  GtkWidget *spacer1 = gtk_label_new ("      ");
  gtk_box_append (GTK_BOX (top_row), spacer1);

  page->indicators[IND_R1] = make_indicator (page, IND_R1);
  gtk_box_append (GTK_BOX (top_row), page->indicators[IND_R1]);

  page->indicators[IND_R2] = make_indicator (page, IND_R2);
  gtk_box_append (GTK_BOX (top_row), page->indicators[IND_R2]);

  gtk_box_append (GTK_BOX (test_box), top_row);

  /* Horizontal band: D-Pad  ─  Select/Home/Start  ─  Face buttons */
  GtkWidget *mid_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 60);
  gtk_widget_set_halign (mid_row, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_top (mid_row, 8);

  /* D-Pad 3×3 grid */
  GtkWidget *dpad_grid = gtk_grid_new ();
  gtk_grid_set_row_spacing (GTK_GRID (dpad_grid), 2);
  gtk_grid_set_column_spacing (GTK_GRID (dpad_grid), 2);

  page->indicators[IND_DPAD_UP] = make_indicator (page, IND_DPAD_UP);
  gtk_grid_attach (GTK_GRID (dpad_grid), page->indicators[IND_DPAD_UP], 1, 0, 1, 1);

  page->indicators[IND_DPAD_LEFT] = make_indicator (page, IND_DPAD_LEFT);
  gtk_grid_attach (GTK_GRID (dpad_grid), page->indicators[IND_DPAD_LEFT], 0, 1, 1, 1);

  page->indicators[IND_DPAD_RIGHT] = make_indicator (page, IND_DPAD_RIGHT);
  gtk_grid_attach (GTK_GRID (dpad_grid), page->indicators[IND_DPAD_RIGHT], 2, 1, 1, 1);

  page->indicators[IND_DPAD_DOWN] = make_indicator (page, IND_DPAD_DOWN);
  gtk_grid_attach (GTK_GRID (dpad_grid), page->indicators[IND_DPAD_DOWN], 1, 2, 1, 1);

  gtk_box_append (GTK_BOX (mid_row), dpad_grid);

  /* Center row: Select  Home  Start */
  GtkWidget *center_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_set_valign (center_row, GTK_ALIGN_CENTER);

  page->indicators[IND_SELECT] = make_indicator (page, IND_SELECT);
  gtk_box_append (GTK_BOX (center_row), page->indicators[IND_SELECT]);

  page->indicators[IND_GUIDE] = make_indicator (page, IND_GUIDE);
  gtk_box_append (GTK_BOX (center_row), page->indicators[IND_GUIDE]);

  page->indicators[IND_START] = make_indicator (page, IND_START);
  gtk_box_append (GTK_BOX (center_row), page->indicators[IND_START]);

  gtk_box_append (GTK_BOX (mid_row), center_row);

  /* Face buttons 3×3 grid */
  GtkWidget *face_grid = gtk_grid_new ();
  gtk_grid_set_row_spacing (GTK_GRID (face_grid), 2);
  gtk_grid_set_column_spacing (GTK_GRID (face_grid), 2);

  page->indicators[IND_Y] = make_indicator (page, IND_Y);
  gtk_grid_attach (GTK_GRID (face_grid), page->indicators[IND_Y], 1, 0, 1, 1);

  page->indicators[IND_X] = make_indicator (page, IND_X);
  gtk_grid_attach (GTK_GRID (face_grid), page->indicators[IND_X], 0, 1, 1, 1);

  page->indicators[IND_B] = make_indicator (page, IND_B);
  gtk_grid_attach (GTK_GRID (face_grid), page->indicators[IND_B], 2, 1, 1, 1);

  page->indicators[IND_A] = make_indicator (page, IND_A);
  gtk_grid_attach (GTK_GRID (face_grid), page->indicators[IND_A], 1, 2, 1, 1);

  gtk_box_append (GTK_BOX (mid_row), face_grid);

  gtk_box_append (GTK_BOX (test_box), mid_row);

  /* Analog sticks row with LS/RS overlay labels */
  GtkWidget *sticks_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 24);
  gtk_widget_set_halign (sticks_row, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_top (sticks_row, 8);

  /* Left stick with "LS" overlay */
  GtkWidget *l_overlay = gtk_overlay_new ();
  page->analog_l_canvas = gtk_drawing_area_new ();
  gtk_widget_set_size_request (page->analog_l_canvas, 100, 100);
  g_object_set_data (G_OBJECT (page->analog_l_canvas), "stick-id",
    GINT_TO_POINTER (STICK_LEFT));
  gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (page->analog_l_canvas),
    on_stick_draw, page, NULL);
  gtk_overlay_set_child (GTK_OVERLAY (l_overlay), page->analog_l_canvas);
  GtkWidget *ls_label = gtk_label_new ("LS");
  gtk_widget_set_halign (ls_label, GTK_ALIGN_START);
  gtk_widget_set_valign (ls_label, GTK_ALIGN_START);
  gtk_widget_set_margin_start (ls_label, 2);
  gtk_widget_set_margin_top (ls_label, 2);
  gtk_overlay_add_overlay (GTK_OVERLAY (l_overlay), ls_label);
  gtk_box_append (GTK_BOX (sticks_row), l_overlay);

  /* Right stick with "RS" overlay */
  GtkWidget *r_overlay = gtk_overlay_new ();
  page->analog_r_canvas = gtk_drawing_area_new ();
  gtk_widget_set_size_request (page->analog_r_canvas, 100, 100);
  g_object_set_data (G_OBJECT (page->analog_r_canvas), "stick-id",
    GINT_TO_POINTER (STICK_RIGHT));
  gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (page->analog_r_canvas),
    on_stick_draw, page, NULL);
  gtk_overlay_set_child (GTK_OVERLAY (r_overlay), page->analog_r_canvas);
  GtkWidget *rs_label = gtk_label_new ("RS");
  gtk_widget_set_halign (rs_label, GTK_ALIGN_END);
  gtk_widget_set_valign (rs_label, GTK_ALIGN_START);
  gtk_widget_set_margin_end (rs_label, 2);
  gtk_widget_set_margin_top (rs_label, 2);
  gtk_overlay_add_overlay (GTK_OVERLAY (r_overlay), rs_label);
  gtk_box_append (GTK_BOX (sticks_row), r_overlay);

  gtk_box_append (GTK_BOX (test_box), sticks_row);

  /* Extra buttons (M1–M4) under the sticks */
  GtkWidget *extra_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 20);
  gtk_widget_set_halign (extra_row, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_top (extra_row, 2);

  page->indicators[IND_M1] = make_indicator (page, IND_M1);
  gtk_box_append (GTK_BOX (extra_row), page->indicators[IND_M1]);
  page->indicators[IND_M2] = make_indicator (page, IND_M2);
  gtk_box_append (GTK_BOX (extra_row), page->indicators[IND_M2]);
  page->indicators[IND_M3] = make_indicator (page, IND_M3);
  gtk_box_append (GTK_BOX (extra_row), page->indicators[IND_M3]);
  page->indicators[IND_M4] = make_indicator (page, IND_M4);
  gtk_box_append (GTK_BOX (extra_row), page->indicators[IND_M4]);

  gtk_box_append (GTK_BOX (test_box), extra_row);

  /* Stick values */
  GtkWidget *vals_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 24);
  gtk_widget_set_halign (vals_row, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_top (vals_row, 4);
  page->axis_lx = gtk_label_new ("LX: 0  LY: 0");
  gtk_box_append (GTK_BOX (vals_row), page->axis_lx);
  page->axis_rx = gtk_label_new ("RX: 0  RY: 0");
  gtk_box_append (GTK_BOX (vals_row), page->axis_rx);
  gtk_box_append (GTK_BOX (test_box), vals_row);

  /* Hint */
  GtkWidget *hint = gtk_label_new (
    tr ("Press any button on the controller"));
  gtk_widget_add_css_class (hint, "dim-label");
  gtk_box_append (GTK_BOX (test_box), hint);

  return test_box;
}

static GtkWidget *
build_controller_test_layout_arcade (HardwarePage *page)
{
  GtkWidget *test_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_set_margin_start (test_box, 16);
  gtk_widget_set_margin_end (test_box, 16);
  gtk_widget_set_margin_top (test_box, 8);
  gtk_widget_set_margin_bottom (test_box, 8);

  /* Top row: share, home, options, L3, R3 */
  GtkWidget *top_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_set_halign (top_row, GTK_ALIGN_CENTER);

  page->indicators[IND_SELECT] = make_indicator (page, IND_SELECT);
  gtk_box_append (GTK_BOX (top_row), page->indicators[IND_SELECT]);

  page->indicators[IND_GUIDE] = make_indicator (page, IND_GUIDE);
  gtk_box_append (GTK_BOX (top_row), page->indicators[IND_GUIDE]);

  page->indicators[IND_START] = make_indicator (page, IND_START);
  gtk_box_append (GTK_BOX (top_row), page->indicators[IND_START]);

  page->indicators[IND_L3] = make_indicator (page, IND_L3);
  gtk_box_append (GTK_BOX (top_row), page->indicators[IND_L3]);

  page->indicators[IND_R3] = make_indicator (page, IND_R3);
  gtk_box_append (GTK_BOX (top_row), page->indicators[IND_R3]);

  gtk_box_append (GTK_BOX (test_box), top_row);

  /* Middle area: Joystick (left) + 2 rows of 4 buttons (right) */
  GtkWidget *mid_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 40);
  gtk_widget_set_halign (mid_row, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_top (mid_row, 8);

  /* Joystick (left side, large) */
  page->analog_l_canvas = gtk_drawing_area_new ();
  gtk_widget_set_size_request (page->analog_l_canvas, 120, 120);
  g_object_set_data (G_OBJECT (page->analog_l_canvas), "stick-id",
    GINT_TO_POINTER (STICK_LEFT));
  gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (page->analog_l_canvas),
    on_stick_draw, page, NULL);
  gtk_box_append (GTK_BOX (mid_row), page->analog_l_canvas);

  /* Button grid (right side) - 2 rows x 4 cols with first col stagger */
  GtkWidget *btn_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_valign (btn_box, GTK_ALIGN_CENTER);

  /* Top row: L1, X, Y, R1 (with L1 slightly lower) */
  GtkWidget *row_top = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  page->indicators[IND_L1] = make_indicator (page, IND_L1);
  gtk_widget_set_margin_top (page->indicators[IND_L1], 6);
  gtk_box_append (GTK_BOX (row_top), page->indicators[IND_L1]);
  page->indicators[IND_X] = make_indicator (page, IND_X);
  gtk_box_append (GTK_BOX (row_top), page->indicators[IND_X]);
  page->indicators[IND_Y] = make_indicator (page, IND_Y);
  gtk_box_append (GTK_BOX (row_top), page->indicators[IND_Y]);
  page->indicators[IND_R1] = make_indicator (page, IND_R1);
  gtk_box_append (GTK_BOX (row_top), page->indicators[IND_R1]);
  gtk_box_append (GTK_BOX (btn_box), row_top);

  /* Bottom row: L2, A, B, R2 */
  GtkWidget *row_bot = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  page->indicators[IND_L2] = make_indicator (page, IND_L2);
  gtk_box_append (GTK_BOX (row_bot), page->indicators[IND_L2]);
  page->indicators[IND_A] = make_indicator (page, IND_A);
  gtk_box_append (GTK_BOX (row_bot), page->indicators[IND_A]);
  page->indicators[IND_B] = make_indicator (page, IND_B);
  gtk_box_append (GTK_BOX (row_bot), page->indicators[IND_B]);
  page->indicators[IND_R2] = make_indicator (page, IND_R2);
  gtk_box_append (GTK_BOX (row_bot), page->indicators[IND_R2]);
  gtk_box_append (GTK_BOX (btn_box), row_bot);

  gtk_box_append (GTK_BOX (mid_row), btn_box);
  gtk_box_append (GTK_BOX (test_box), mid_row);

  /* D-Pad (bottom, smaller and less prominent) */
  GtkWidget *dpad_label = gtk_label_new (tr ("D-Pad"));
  gtk_widget_add_css_class (dpad_label, "dim-label");
  gtk_widget_set_halign (dpad_label, GTK_ALIGN_START);
  gtk_box_append (GTK_BOX (test_box), dpad_label);

  GtkWidget *dpad_grid = gtk_grid_new ();
  gtk_grid_set_row_spacing (GTK_GRID (dpad_grid), 2);
  gtk_grid_set_column_spacing (GTK_GRID (dpad_grid), 2);
  gtk_widget_set_halign (dpad_grid, GTK_ALIGN_START);

  page->indicators[IND_DPAD_UP] = make_indicator (page, IND_DPAD_UP);
  gtk_grid_attach (GTK_GRID (dpad_grid), page->indicators[IND_DPAD_UP], 1, 0, 1, 1);
  page->indicators[IND_DPAD_LEFT] = make_indicator (page, IND_DPAD_LEFT);
  gtk_grid_attach (GTK_GRID (dpad_grid), page->indicators[IND_DPAD_LEFT], 0, 1, 1, 1);
  page->indicators[IND_DPAD_RIGHT] = make_indicator (page, IND_DPAD_RIGHT);
  gtk_grid_attach (GTK_GRID (dpad_grid), page->indicators[IND_DPAD_RIGHT], 2, 1, 1, 1);
  page->indicators[IND_DPAD_DOWN] = make_indicator (page, IND_DPAD_DOWN);
  gtk_grid_attach (GTK_GRID (dpad_grid), page->indicators[IND_DPAD_DOWN], 1, 2, 1, 1);

  gtk_box_append (GTK_BOX (test_box), dpad_grid);

  /* Extra buttons (M1–M4) */
  GtkWidget *extra_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_set_halign (extra_row, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_top (extra_row, 4);

  page->indicators[IND_M1] = make_indicator (page, IND_M1);
  gtk_box_append (GTK_BOX (extra_row), page->indicators[IND_M1]);
  page->indicators[IND_M2] = make_indicator (page, IND_M2);
  gtk_box_append (GTK_BOX (extra_row), page->indicators[IND_M2]);
  page->indicators[IND_M3] = make_indicator (page, IND_M3);
  gtk_box_append (GTK_BOX (extra_row), page->indicators[IND_M3]);
  page->indicators[IND_M4] = make_indicator (page, IND_M4);
  gtk_box_append (GTK_BOX (extra_row), page->indicators[IND_M4]);

  gtk_box_append (GTK_BOX (test_box), extra_row);

  /* Stick values */
  GtkWidget *vals_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 24);
  gtk_widget_set_halign (vals_row, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_top (vals_row, 4);
  page->axis_lx = gtk_label_new ("LX: 0  LY: 0");
  gtk_box_append (GTK_BOX (vals_row), page->axis_lx);
  page->axis_rx = gtk_label_new ("RX: 0  RY: 0");
  gtk_box_append (GTK_BOX (vals_row), page->axis_rx);
  gtk_box_append (GTK_BOX (test_box), vals_row);

  /* Hint */
  GtkWidget *hint = gtk_label_new (
    tr ("Press any button on the controller"));
  gtk_widget_add_css_class (hint, "dim-label");
  gtk_box_append (GTK_BOX (test_box), hint);

  return test_box;
}

static void
on_controller_website_clicked (GtkButton *btn, gpointer user_data)
{
  (void) btn;
  HardwarePage *page = user_data;
  if (!page->controller_url) return;
  GError *err = NULL;
  g_app_info_launch_default_for_uri (page->controller_url, NULL, &err);
  if (err)
    {
      g_error_free (err);
      dashboard_show_toast (tr ("Could not open URL"));
    }
}

void
hw_controller_build_detail (HardwarePage *page)
{
  page->controller_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_start (page->controller_box, 16);
  gtk_widget_set_margin_end (page->controller_box, 16);
  gtk_widget_set_margin_top (page->controller_box, 16);

  /* Branding */
  GtkWidget *branding_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 16);

  GtkWidget *brand_icon = gtk_image_new_from_icon_name ("joystick-arcade-symbolic");
  gtk_image_set_pixel_size (GTK_IMAGE (brand_icon), 64);
  gtk_widget_set_valign (brand_icon, GTK_ALIGN_START);

  page->controller_name_label = gtk_label_new ("");
  gtk_widget_add_css_class (page->controller_name_label, "title-1");
  gtk_widget_set_halign (page->controller_name_label, GTK_ALIGN_START);

  page->controller_info_label = gtk_label_new ("");
  gtk_widget_add_css_class (page->controller_info_label, "dim-label");
  gtk_widget_set_halign (page->controller_info_label, GTK_ALIGN_START);

  GtkWidget *branding_text = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_set_hexpand (branding_text, TRUE);
  gtk_box_append (GTK_BOX (branding_text), page->controller_name_label);
  gtk_box_append (GTK_BOX (branding_text), page->controller_info_label);

  page->controller_website_btn = gtk_button_new_with_label (
    tr ("Official Website"));
  gtk_widget_set_valign (page->controller_website_btn, GTK_ALIGN_START);
  gtk_widget_set_margin_top (page->controller_website_btn, 4);
  gtk_widget_add_css_class (page->controller_website_btn, "text-button");
  gtk_widget_set_visible (page->controller_website_btn, FALSE);
  g_signal_connect (page->controller_website_btn, "clicked",
    G_CALLBACK (on_controller_website_clicked), page);

  gtk_box_append (GTK_BOX (branding_box), brand_icon);
  gtk_box_append (GTK_BOX (branding_box), branding_text);
  gtk_box_append (GTK_BOX (branding_box), page->controller_website_btn);
  gtk_box_append (GTK_BOX (page->controller_box), branding_box);

  /* Stack switcher with two views */
  GtkWidget *stack = adw_view_stack_new ();
  GtkWidget *switcher = adw_view_switcher_new ();
  adw_view_switcher_set_stack (ADW_VIEW_SWITCHER (switcher), ADW_VIEW_STACK (stack));
  adw_view_switcher_set_policy (ADW_VIEW_SWITCHER (switcher), ADW_VIEW_SWITCHER_POLICY_WIDE);

  /* Settings page */
  GtkWidget *settings_scroll = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (settings_scroll),
    GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_propagate_natural_width (
    GTK_SCROLLED_WINDOW (settings_scroll), TRUE);
  gtk_scrolled_window_set_propagate_natural_height (
    GTK_SCROLLED_WINDOW (settings_scroll), TRUE);

  GtkWidget *settings_content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_valign (settings_content, GTK_ALIGN_START);
  gtk_widget_set_margin_top (settings_content, 24);

  /* --- Settings group --- */
  AdwPreferencesGroup *settings_group = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  adw_preferences_group_set_title (settings_group,
    tr ("Settings"));

  GtkWidget *settings_placeholder = gtk_label_new (
    tr ("No settings available yet."));
  gtk_widget_add_css_class (settings_placeholder, "dim-label");
  gtk_widget_set_halign (settings_placeholder, GTK_ALIGN_START);
  gtk_widget_set_hexpand (settings_placeholder, TRUE);
  adw_preferences_group_add (settings_group, settings_placeholder);

  gtk_box_append (GTK_BOX (settings_content), GTK_WIDGET (settings_group));

  /* --- Specifications group --- */
  AdwPreferencesGroup *specs_group = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  adw_preferences_group_set_title (specs_group,
    tr ("Specifications"));

  GtkWidget *specs_placeholder = gtk_label_new (
    tr ("No specification data available."));
  gtk_widget_add_css_class (specs_placeholder, "dim-label");
  gtk_widget_set_halign (specs_placeholder, GTK_ALIGN_START);
  gtk_widget_set_hexpand (specs_placeholder, TRUE);
  adw_preferences_group_add (specs_group, specs_placeholder);

  gtk_box_append (GTK_BOX (settings_content), GTK_WIDGET (specs_group));

  /* --- Lighting Controls --- */
  AdwPreferencesGroup *lighting_group = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  GtkWidget *lighting_header = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget *lighting_title = gtk_label_new (
    tr ("Lighting Controls"));
  gtk_widget_add_css_class (lighting_title, "heading");
  gtk_widget_set_halign (lighting_title, GTK_ALIGN_START);
  gtk_widget_set_hexpand (lighting_title, TRUE);
  gtk_box_append (GTK_BOX (lighting_header), lighting_title);

  if (g_find_program_in_path ("openrgb"))
    {
      GtkWidget *openrgb_btn = gtk_button_new_with_label (
        tr ("Edit on OpenRGB"));
      gtk_widget_add_css_class (openrgb_btn, "accent");
      gtk_widget_set_valign (openrgb_btn, GTK_ALIGN_CENTER);
      gtk_box_append (GTK_BOX (lighting_header), openrgb_btn);
      g_signal_connect (openrgb_btn, "clicked", G_CALLBACK (hw_openrgb_launch), NULL);
    }

  adw_preferences_group_add (lighting_group, lighting_header);

  GtkWidget *lighting_status = gtk_label_new (
    tr ("The app couldn't find any light source to manage."));
  gtk_widget_set_halign (lighting_status, GTK_ALIGN_START);
  gtk_widget_set_hexpand (lighting_status, TRUE);
  gtk_label_set_wrap (GTK_LABEL (lighting_status), TRUE);
  gtk_label_set_max_width_chars (GTK_LABEL (lighting_status), 60);
  adw_preferences_group_add (lighting_group, lighting_status);

  gtk_box_append (GTK_BOX (settings_content), GTK_WIDGET (lighting_group));

  /* --- Extra Buttons group --- */
  page->extra_buttons_group = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  adw_preferences_group_set_title (page->extra_buttons_group,
    tr ("Extra Buttons"));

  page->extra_buttons_none = gtk_label_new (
    tr ("No extra buttons detected. "
      "This controller does not have programmable buttons."));
  gtk_widget_add_css_class (page->extra_buttons_none, "dim-label");
  gtk_widget_set_halign (page->extra_buttons_none, GTK_ALIGN_START);
  gtk_widget_set_hexpand (page->extra_buttons_none, TRUE);
  adw_preferences_group_add (page->extra_buttons_group, page->extra_buttons_none);

  gtk_box_append (GTK_BOX (settings_content), GTK_WIDGET (page->extra_buttons_group));

  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (settings_scroll), settings_content);
  adw_view_stack_add_titled_with_icon (ADW_VIEW_STACK (stack), settings_scroll, "settings",
    tr ("Settings"), "emblem-system-symbolic");

  /* Test page */
  page->controller_test_stack = gtk_stack_new ();
  gtk_stack_set_transition_type (GTK_STACK (page->controller_test_stack),
    GTK_STACK_TRANSITION_TYPE_SLIDE_LEFT_RIGHT);

  GtkWidget *test_page_standard = build_controller_test_layout_standard (page);

  /* Snapshot standard layout pointers before arcade overwrites them */
  memcpy (page->indicators_standard, page->indicators, sizeof (page->indicators_standard));
  page->analog_l_canvas_standard = page->analog_l_canvas;
  page->axis_lx_standard = page->axis_lx;
  page->axis_rx_standard = page->axis_rx;

  gtk_stack_add_named (GTK_STACK (page->controller_test_stack),
    test_page_standard, "controller");

  GtkWidget *test_page_arcade = build_controller_test_layout_arcade (page);

  /* Snapshot arcade layout pointers */
  memcpy (page->indicators_arcade, page->indicators, sizeof (page->indicators_arcade));
  page->analog_l_canvas_arcade = page->analog_l_canvas;
  page->axis_lx_arcade = page->axis_lx;
  page->axis_rx_arcade = page->axis_rx;

  /* Restore standard layout pointers as default */
  memcpy (page->indicators, page->indicators_standard, sizeof (page->indicators));
  page->analog_l_canvas = page->analog_l_canvas_standard;
  page->axis_lx = page->axis_lx_standard;
  page->axis_rx = page->axis_rx_standard;

  gtk_stack_add_named (GTK_STACK (page->controller_test_stack),
    test_page_arcade, "arcade");

  gtk_stack_set_visible_child_name (GTK_STACK (page->controller_test_stack), "controller");

  adw_view_stack_add_titled_with_icon (ADW_VIEW_STACK (stack),
    page->controller_test_stack, "test",
    tr ("Test"), "joystick-symbolic");

  gtk_box_append (GTK_BOX (page->controller_box), switcher);
  gtk_box_append (GTK_BOX (page->controller_box), stack);
  gtk_stack_add_named (page->detail_stack, page->controller_box, "controller");
}

void
hw_controller_close (HardwarePage *page)
{
  if (page->controller_watch_id)
    {
      g_source_remove (page->controller_watch_id);
      page->controller_watch_id = 0;
    }
  if (page->controller_fd >= 0)
    {
      close (page->controller_fd);
      page->controller_fd = -1;
    }
  for (int i = 0; i < IND_COUNT; i++)
    if (page->indicators[i])
      gtk_widget_remove_css_class (page->indicators[i], "ctrl-indicator-active");
  g_clear_pointer (&page->ctrl_state, g_free);
}

void
hw_controller_update_extra_buttons (HardwarePage *page)
{
  /* Remove all previous rows from the group (except "none" label which we
     will remove and re-add as needed). We unparent every child first. */
  GtkWidget *child = gtk_widget_get_first_child (GTK_WIDGET (page->extra_buttons_group));
  while (child)
    {
      GtkWidget *next = gtk_widget_get_next_sibling (child);
      gtk_widget_unparent (child);
      child = next;
    }

  if (page->controller_fd < 0)
    {
      adw_preferences_group_add (page->extra_buttons_group, page->extra_buttons_none);
      gtk_widget_set_visible (page->extra_buttons_none, TRUE);
      return;
    }

  unsigned long key_bits[KEY_MAX / (8 * sizeof (unsigned long)) + 1] = {0};
  if (ioctl (page->controller_fd, EVIOCGBIT (EV_KEY, sizeof (key_bits)), key_bits) < 0)
    {
      adw_preferences_group_add (page->extra_buttons_group, page->extra_buttons_none);
      gtk_widget_set_visible (page->extra_buttons_none, TRUE);
      return;
    }

  static const struct { int code; const char *label; } btns[] = {
    { BTN_TRIGGER_HAPPY1, "M1" },
    { BTN_TRIGGER_HAPPY2, "M2" },
    { BTN_TRIGGER_HAPPY3, "M3" },
    { BTN_TRIGGER_HAPPY4, "M4" },
  };
  int found = 0;

  for (int i = 0; i < 4; i++)
    {
      if (TEST_BIT (btns[i].code, key_bits))
        {
          found++;
          GtkWidget *row = adw_action_row_new ();
          adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), btns[i].label);
          char subtitle[128];
          g_snprintf (subtitle, sizeof (subtitle), "%s (code 0x%x)",
            tr ("Detected"), btns[i].code);
          adw_action_row_set_subtitle (ADW_ACTION_ROW (row), subtitle);
          adw_preferences_group_add (page->extra_buttons_group,
            GTK_WIDGET (row));
        }
    }

  if (found == 0)
    {
      adw_preferences_group_add (page->extra_buttons_group, page->extra_buttons_none);
      gtk_widget_set_visible (page->extra_buttons_none, TRUE);
    }
}

void
hw_controller_show (HardwarePage *page)
{
  /* Resolve which device is selected by the caller; use its index data. */
  gtk_stack_set_visible_child_name (page->detail_stack, "controller");

  /* Layout switching (Arcade vs standard) is driven by device_categories. */
  int idx = gtk_list_box_get_selected_row (GTK_LIST_BOX (page->device_list))
    ? gtk_list_box_row_get_index (gtk_list_box_get_selected_row (GTK_LIST_BOX (page->device_list)))
    : -1;
  if (idx < 0 || idx >= page->n_devices) return;

  const char *cat = page->device_categories[idx];
  if (cat && g_strcmp0 (cat, "Arcade") == 0)
    {
      gtk_stack_set_visible_child_name (GTK_STACK (page->controller_test_stack), "arcade");
      memcpy (page->indicators, page->indicators_arcade, sizeof (page->indicators));
      page->analog_l_canvas = page->analog_l_canvas_arcade;
      page->axis_lx = page->axis_lx_arcade;
      page->axis_rx = page->axis_rx_arcade;
    }
  else
    {
      gtk_stack_set_visible_child_name (GTK_STACK (page->controller_test_stack), "controller");
      memcpy (page->indicators, page->indicators_standard, sizeof (page->indicators));
      page->analog_l_canvas = page->analog_l_canvas_standard;
      page->axis_lx = page->axis_lx_standard;
      page->axis_rx = page->axis_rx_standard;
    }
  gtk_label_set_text (GTK_LABEL (page->controller_name_label),
    page->device_names[idx]);
  gtk_label_set_text (GTK_LABEL (page->controller_info_label),
    page->device_info[idx] && page->device_info[idx][0]
      ? page->device_info[idx] : "");

  /* Show/hide official website button */
  g_free (page->controller_url);
  page->controller_url = page->device_urls[idx]
    ? g_strdup (page->device_urls[idx]) : NULL;
  gtk_widget_set_visible (page->controller_website_btn,
    page->controller_url != NULL);

  /* Detect manufacturer for button labels */
  const char *name = page->device_names[idx];
  const char **labels = indicator_labels_xbox;
  if (strstr (name, "PlayStation") || strstr (name, "PS") ||
      strstr (name, "DUALSHOCK") || strstr (name, "DualSense") ||
      strstr (name, "PS4") || strstr (name, "PS5"))
    labels = indicator_labels_ps;
  else if (strstr (name, "Nintendo") || strstr (name, "Switch") ||
           strstr (name, "Pro Controller") || strstr (name, "Joy-Con") ||
           strstr (name, "Steam Controller"))
    labels = indicator_labels_generic;
  update_indicator_labels (page, labels);

  const char *path = page->device_paths[idx];
  page->controller_fd = open (path, O_RDONLY | O_NONBLOCK);
  if (page->controller_fd >= 0)
    {
      page->ctrl_state = g_new0 (ControllerState, 1);
      page->controller_watch_id = g_unix_fd_add (page->controller_fd,
        G_IO_IN, hw_controller_event, page);
    }
  else
    g_warning ("Failed to open controller device %s: %s",
      path, g_strerror (errno));

  hw_controller_update_extra_buttons (page);
}
