#include "page-mangohud.h"
#include "page-dashboard.h"

#include <string.h>

static void
collect_bool (GString *buf, const char *key, gboolean val)
{
  g_string_append_printf (buf, "%s=%s\n", key, val ? "true" : "false");
}

static const char *POSITION_VALUES[] = {
  "top-left", "top-right", "bottom-left", "bottom-right",
  "top-center", "bottom-center", "middle-left", "middle-right"
};

static char *
get_mangohud_config_path (void)
{
  const char *home = g_get_home_dir ();
  return g_build_filename (home, ".config", "MangoHud", "MangoHud.conf", NULL);
}

static void
on_save_clicked (GtkButton *btn, MangoHudPage *page)
{
  (void) btn;

  char *cfg_path = get_mangohud_config_path ();
  char *dir = g_path_get_dirname (cfg_path);
  g_mkdir_with_parents (dir, 0755);
  g_free (dir);

  GString *buf = g_string_new ("");

  collect_bool (buf, "fps",           adw_switch_row_get_active (page->show_fps));
  collect_bool (buf, "frame_timing",  adw_switch_row_get_active (page->show_frame_timing));
  collect_bool (buf, "cpu_stats",     adw_switch_row_get_active (page->show_cpu));
  collect_bool (buf, "cpu_temp",      adw_switch_row_get_active (page->show_cpu_temp));
  collect_bool (buf, "gpu_stats",     adw_switch_row_get_active (page->show_gpu));
  collect_bool (buf, "gpu_temp",      adw_switch_row_get_active (page->show_gpu_temp));
  collect_bool (buf, "vram",          adw_switch_row_get_active (page->show_vram));
  collect_bool (buf, "ram",           adw_switch_row_get_active (page->show_ram));
  collect_bool (buf, "procmem",       adw_switch_row_get_active (page->show_procmem));
  collect_bool (buf, "io_stats",      adw_switch_row_get_active (page->show_io));
  collect_bool (buf, "power",         adw_switch_row_get_active (page->show_power));
  collect_bool (buf, "engine_version", adw_switch_row_get_active (page->show_engine));
  collect_bool (buf, "vulkan_driver", adw_switch_row_get_active (page->show_vulkan_driver));
  collect_bool (buf, "wine",          adw_switch_row_get_active (page->show_wine));
  collect_bool (buf, "histogram",     adw_switch_row_get_active (page->show_histogram));

  {
    guint idx = adw_combo_row_get_selected (page->position);
    const char *pos = (idx < G_N_ELEMENTS (POSITION_VALUES))
                      ? POSITION_VALUES[idx] : "top-left";
    g_string_append_printf (buf, "position=%s\n", pos);
  }

  g_string_append_printf (buf, "offset_x=%d\n",
    (int) adw_spin_row_get_value (page->offset_x));
  g_string_append_printf (buf, "offset_y=%d\n",
    (int) adw_spin_row_get_value (page->offset_y));
  g_string_append_printf (buf, "font_size=%d\n",
    (int) adw_spin_row_get_value (page->font_size));
  g_string_append_printf (buf, "table_columns=%d\n",
    (int) adw_spin_row_get_value (page->table_columns));
  g_string_append_printf (buf, "background_alpha=%d\n",
    (int) adw_spin_row_get_value (page->background_alpha));
  g_string_append_printf (buf, "fps_limit=%d\n",
    (int) adw_spin_row_get_value (page->fps_limit));

  {
    const char *text = gtk_editable_get_text (GTK_EDITABLE (page->custom_text_center));
    if (text && text[0] != '\0')
      g_string_append_printf (buf, "custom_text_center=%s\n", text);
  }
  {
    const char *text = gtk_editable_get_text (GTK_EDITABLE (page->custom_text_left));
    if (text && text[0] != '\0')
      g_string_append_printf (buf, "custom_text_left=%s\n", text);
  }
  {
    const char *text = gtk_editable_get_text (GTK_EDITABLE (page->custom_text_right));
    if (text && text[0] != '\0')
      g_string_append_printf (buf, "custom_text_right=%s\n", text);
  }

  GError *err = NULL;
  if (!g_file_set_contents (cfg_path, buf->str, -1, &err))
    {
      dashboard_show_toast (err->message);
      g_error_free (err);
    }
  else
    {
      char *msg = g_strdup_printf ("Saved to %s", cfg_path);
      dashboard_show_toast (msg);
      g_free (msg);
    }

  g_string_free (buf, TRUE);
  g_free (cfg_path);
}

static gboolean
parse_bool (const char *value)
{
  if (!value) return FALSE;
  return g_strcmp0 (value, "1") == 0 || g_strcmp0 (value, "yes") == 0
      || g_strcmp0 (value, "true") == 0;
}

static int
find_position_index (const char *value)
{
  if (!value) return 0;
  for (guint i = 0; i < G_N_ELEMENTS (POSITION_VALUES); i++)
    if (g_strcmp0 (value, POSITION_VALUES[i]) == 0)
      return i;
  return 0;
}

static void
load_config (MangoHudPage *page)
{
  char *cfg_path = get_mangohud_config_path ();
  char *contents = NULL;
  GError *err = NULL;

  if (!g_file_get_contents (cfg_path, &contents, NULL, &err))
    {
      g_error_free (err);
      g_free (cfg_path);
      return;
    }

  char **lines = g_strsplit (contents, "\n", -1);
  for (int i = 0; lines[i]; i++)
    {
      char *line = g_strstrip (lines[i]);
      if (line[0] == '\0' || line[0] == ';' || line[0] == '#')
        continue;

      char *eq = strchr (line, '=');
      if (!eq) continue;

      *eq = '\0';
      char *key = g_strstrip (line);
      char *value = g_strstrip (eq + 1);

      if (g_strcmp0 (key, "fps") == 0)
        adw_switch_row_set_active (page->show_fps, parse_bool (value));
      else if (g_strcmp0 (key, "frame_timing") == 0)
        adw_switch_row_set_active (page->show_frame_timing, parse_bool (value));
      else if (g_strcmp0 (key, "cpu_stats") == 0)
        adw_switch_row_set_active (page->show_cpu, parse_bool (value));
      else if (g_strcmp0 (key, "cpu_temp") == 0)
        adw_switch_row_set_active (page->show_cpu_temp, parse_bool (value));
      else if (g_strcmp0 (key, "gpu_stats") == 0)
        adw_switch_row_set_active (page->show_gpu, parse_bool (value));
      else if (g_strcmp0 (key, "gpu_temp") == 0)
        adw_switch_row_set_active (page->show_gpu_temp, parse_bool (value));
      else if (g_strcmp0 (key, "vram") == 0)
        adw_switch_row_set_active (page->show_vram, parse_bool (value));
      else if (g_strcmp0 (key, "ram") == 0)
        adw_switch_row_set_active (page->show_ram, parse_bool (value));
      else if (g_strcmp0 (key, "procmem") == 0)
        adw_switch_row_set_active (page->show_procmem, parse_bool (value));
      else if (g_strcmp0 (key, "io_stats") == 0)
        adw_switch_row_set_active (page->show_io, parse_bool (value));
      else if (g_strcmp0 (key, "power") == 0)
        adw_switch_row_set_active (page->show_power, parse_bool (value));
      else if (g_strcmp0 (key, "engine_version") == 0)
        adw_switch_row_set_active (page->show_engine, parse_bool (value));
      else if (g_strcmp0 (key, "vulkan_driver") == 0)
        adw_switch_row_set_active (page->show_vulkan_driver, parse_bool (value));
      else if (g_strcmp0 (key, "wine") == 0)
        adw_switch_row_set_active (page->show_wine, parse_bool (value));
      else if (g_strcmp0 (key, "histogram") == 0)
        adw_switch_row_set_active (page->show_histogram, parse_bool (value));
      else if (g_strcmp0 (key, "position") == 0)
        adw_combo_row_set_selected (page->position, find_position_index (value));
      else if (g_strcmp0 (key, "offset_x") == 0)
        adw_spin_row_set_value (page->offset_x, atoi (value));
      else if (g_strcmp0 (key, "offset_y") == 0)
        adw_spin_row_set_value (page->offset_y, atoi (value));
      else if (g_strcmp0 (key, "font_size") == 0)
        adw_spin_row_set_value (page->font_size, atoi (value));
      else if (g_strcmp0 (key, "table_columns") == 0)
        adw_spin_row_set_value (page->table_columns, atoi (value));
      else if (g_strcmp0 (key, "background_alpha") == 0)
        adw_spin_row_set_value (page->background_alpha, atoi (value));
      else if (g_strcmp0 (key, "fps_limit") == 0)
        adw_spin_row_set_value (page->fps_limit, atoi (value));
      else if (g_strcmp0 (key, "custom_text_center") == 0)
        gtk_editable_set_text (GTK_EDITABLE (page->custom_text_center), value);
      else if (g_strcmp0 (key, "custom_text_left") == 0)
        gtk_editable_set_text (GTK_EDITABLE (page->custom_text_left), value);
      else if (g_strcmp0 (key, "custom_text_right") == 0)
        gtk_editable_set_text (GTK_EDITABLE (page->custom_text_right), value);
    }

  g_strfreev (lines);
  g_free (contents);
  g_free (cfg_path);
}

static void
on_reset_clicked (GtkButton *btn, MangoHudPage *page)
{
  (void) btn;

  adw_switch_row_set_active (page->show_fps, TRUE);
  adw_switch_row_set_active (page->show_frame_timing, FALSE);
  adw_switch_row_set_active (page->show_cpu, TRUE);
  adw_switch_row_set_active (page->show_cpu_temp, TRUE);
  adw_switch_row_set_active (page->show_gpu, TRUE);
  adw_switch_row_set_active (page->show_gpu_temp, TRUE);
  adw_switch_row_set_active (page->show_vram, TRUE);
  adw_switch_row_set_active (page->show_ram, FALSE);
  adw_switch_row_set_active (page->show_procmem, FALSE);
  adw_switch_row_set_active (page->show_io, FALSE);
  adw_switch_row_set_active (page->show_power, FALSE);
  adw_switch_row_set_active (page->show_engine, FALSE);
  adw_switch_row_set_active (page->show_vulkan_driver, FALSE);
  adw_switch_row_set_active (page->show_wine, FALSE);
  adw_switch_row_set_active (page->show_histogram, FALSE);

  adw_combo_row_set_selected (page->position, 0);
  adw_spin_row_set_value (page->offset_x, 0);
  adw_spin_row_set_value (page->offset_y, 0);
  adw_spin_row_set_value (page->font_size, 24);
  adw_spin_row_set_value (page->table_columns, 3);
  adw_spin_row_set_value (page->background_alpha, 75);
  adw_spin_row_set_value (page->fps_limit, 0);

  gtk_editable_set_text (GTK_EDITABLE (page->custom_text_center), "");
  gtk_editable_set_text (GTK_EDITABLE (page->custom_text_left), "");
  gtk_editable_set_text (GTK_EDITABLE (page->custom_text_right), "");

  dashboard_show_toast ("Defaults restored (not saved)");
}

MangoHudPage *
mangohud_page_new (void)
{
  MangoHudPage *page = g_new0 (MangoHudPage, 1);

  GtkBuilder *builder = gtk_builder_new_from_resource (
    "/io/github/mayrinck/epinephrine/ui/page-mangohud.ui");

  page->box = GTK_WIDGET (gtk_builder_get_object (builder, "mangohud-page"));

  page->show_fps           = ADW_SWITCH_ROW (gtk_builder_get_object (builder, "mh-show-fps"));
  page->show_frame_timing  = ADW_SWITCH_ROW (gtk_builder_get_object (builder, "mh-show-frame-timing"));
  page->show_cpu           = ADW_SWITCH_ROW (gtk_builder_get_object (builder, "mh-show-cpu"));
  page->show_cpu_temp      = ADW_SWITCH_ROW (gtk_builder_get_object (builder, "mh-show-cpu-temp"));
  page->show_gpu           = ADW_SWITCH_ROW (gtk_builder_get_object (builder, "mh-show-gpu"));
  page->show_gpu_temp      = ADW_SWITCH_ROW (gtk_builder_get_object (builder, "mh-show-gpu-temp"));
  page->show_vram          = ADW_SWITCH_ROW (gtk_builder_get_object (builder, "mh-show-vram"));
  page->show_ram           = ADW_SWITCH_ROW (gtk_builder_get_object (builder, "mh-show-ram"));
  page->show_procmem       = ADW_SWITCH_ROW (gtk_builder_get_object (builder, "mh-show-procmem"));
  page->show_io            = ADW_SWITCH_ROW (gtk_builder_get_object (builder, "mh-show-io"));
  page->show_power         = ADW_SWITCH_ROW (gtk_builder_get_object (builder, "mh-show-power"));
  page->show_engine        = ADW_SWITCH_ROW (gtk_builder_get_object (builder, "mh-show-engine"));
  page->show_vulkan_driver = ADW_SWITCH_ROW (gtk_builder_get_object (builder, "mh-show-vulkan-driver"));
  page->show_wine          = ADW_SWITCH_ROW (gtk_builder_get_object (builder, "mh-show-wine"));
  page->show_histogram     = ADW_SWITCH_ROW (gtk_builder_get_object (builder, "mh-show-histogram"));

  page->position   = ADW_COMBO_ROW (gtk_builder_get_object (builder, "mh-position"));
  page->offset_x   = ADW_SPIN_ROW (gtk_builder_get_object (builder, "mh-offset-x"));
  page->offset_y   = ADW_SPIN_ROW (gtk_builder_get_object (builder, "mh-offset-y"));
  page->font_size  = ADW_SPIN_ROW (gtk_builder_get_object (builder, "mh-font-size"));
  page->table_columns = ADW_SPIN_ROW (gtk_builder_get_object (builder, "mh-table-columns"));

  page->background_alpha = ADW_SPIN_ROW (gtk_builder_get_object (builder, "mh-background-alpha"));

  page->fps_limit = ADW_SPIN_ROW (gtk_builder_get_object (builder, "mh-fps-limit"));

  page->custom_text_center = ADW_ENTRY_ROW (gtk_builder_get_object (builder, "mh-custom-text-center"));
  page->custom_text_left   = ADW_ENTRY_ROW (gtk_builder_get_object (builder, "mh-custom-text-left"));
  page->custom_text_right  = ADW_ENTRY_ROW (gtk_builder_get_object (builder, "mh-custom-text-right"));

  page->save_button   = GTK_BUTTON (gtk_builder_get_object (builder, "mh-save-button"));
  page->reset_button  = GTK_BUTTON (gtk_builder_get_object (builder, "mh-reset-button"));

  g_signal_connect (page->save_button, "clicked",
    G_CALLBACK (on_save_clicked), page);
  g_signal_connect (page->reset_button, "clicked",
    G_CALLBACK (on_reset_clicked), page);

  load_config (page);

  g_object_ref_sink (page->box);
  g_object_unref (builder);

  return page;
}
