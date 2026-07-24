#include "page-gamemode.h"
#include "page-dashboard.h"
#include "epi-privilege.h"

#include <string.h>
#include <fcntl.h>

#define TWEAK_MARKER_START "# --- epinephrine-tweaks-start ---\n"
#define TWEAK_MARKER_END   "# --- epinephrine-tweaks-end ---\n"

static const char *
get_gamemode_config_path (void)
{
  const char *home = g_get_home_dir ();
  char *user_path = g_build_filename (home, ".config", "gamemode.ini", NULL);
  if (g_file_test (user_path, G_FILE_TEST_EXISTS))
    {
      g_free (user_path);
      static const char *user_static = NULL;
      if (!user_static)
        user_static = g_build_filename (home, ".config", "gamemode.ini", NULL);
      return user_static;
    }
  g_free (user_path);
  return "/etc/gamemode.ini";
}

static const char *CONFIG_PATH = NULL;

static const char *
config_path (void)
{
  if (!CONFIG_PATH)
    CONFIG_PATH = get_gamemode_config_path ();
  return CONFIG_PATH;
}

static void
tweak_de_detect (gboolean *is_gnome, gboolean *is_kde)
{
  const char *xdg = g_getenv ("XDG_CURRENT_DESKTOP");
  if (!xdg) xdg = "";
  *is_gnome = (strstr (xdg, "GNOME") != NULL) || (strstr (xdg, "gnome") != NULL);
  *is_kde   = (strstr (xdg, "KDE") != NULL) || (strstr (xdg, "kde") != NULL)
              || (strstr (xdg, "Plasma") != NULL);
}

/* ---- Tweak command helpers ---- */

static void
tweak_get_anim (gboolean enable, char **out)
{
  gboolean gnome, kde;
  tweak_de_detect (&gnome, &kde);
  const char *v = enable ? "true" : "false";
  if (gnome)
    *out = g_strdup_printf ("gsettings set org.gnome.desktop.interface enable-animations %s", v);
  else if (kde)
    *out = g_strdup_printf ("kwriteconfig5 --file kwinrc --group Compositing --key Enabled %s && qdbus org.kde.KWin /KWin reconfigure", v);
  else
    *out = g_strdup_printf ("gsettings set org.gnome.desktop.interface enable-animations %s", v);
}

static void
tweak_get_dnd (gboolean enable, char **out)
{
  gboolean gnome, kde;
  tweak_de_detect (&gnome, &kde);
  if (gnome)
    *out = g_strdup_printf ("gsettings set org.gnome.desktop.notifications show-banners %s", enable ? "true" : "false");
  else if (kde)
    *out = g_strdup_printf ("qdbus org.kde.NotificationManager /NotificationManager disableNotifications %s", enable ? "false" : "true");
  else
    *out = g_strdup_printf ("gsettings set org.gnome.desktop.notifications show-banners %s", enable ? "true" : "false");
}

static void
tweak_get_compositor (gboolean enable, char **out)
{
  gboolean gnome, kde;
  tweak_de_detect (&gnome, &kde);
  if (gnome)
    *out = g_strdup (enable
      ? "gsettings set org.gnome.mutter check-alive-timeout 0"
      : "gsettings reset org.gnome.mutter check-alive-timeout");
  else if (kde)
    *out = g_strdup (enable
      ? "qdbus org.kde.KWin /Compositor resume"
      : "qdbus org.kde.KWin /Compositor suspend");
  else
    *out = g_strdup (enable
      ? "gsettings set org.gnome.mutter check-alive-timeout 0"
      : "gsettings reset org.gnome.mutter check-alive-timeout");
}

static void
tweak_get_swappiness (gboolean enable, char **out)
{
  *out = g_strdup (enable
    ? "sysctl vm.swappiness=10"
    : "sysctl vm.swappiness=60");
}

static void
tweak_get_nmi_watchdog (gboolean enable, char **out)
{
  *out = g_strdup (enable
    ? "sysctl kernel.nmi_watchdog=0"
    : "sysctl kernel.nmi_watchdog=1");
}

static void
tweak_get_usb_autosuspend (gboolean enable, char **out)
{
  if (enable)
    *out = g_strdup ("echo -1 > /sys/module/usbcore/parameters/autosuspend");
  else
    *out = g_strdup ("echo 2 > /sys/module/usbcore/parameters/autosuspend");
}

static void
tweak_get_timer_khz (gboolean enable, char **out)
{
  *out = g_strdup (enable
    ? "sysctl kernel.timer_khz=1000"
    : "sysctl kernel.timer_khz=250");
}

static void
tweak_get_disable_wakeup (gboolean enable, char **out)
{
  if (enable)
    *out = g_strdup ("for d in /sys/bus/usb/devices/*/power/wakeup; do echo disabled > \"$d\" 2>/dev/null; done");
  else
    *out = g_strdup ("for d in /sys/bus/usb/devices/*/power/wakeup; do echo enabled > \"$d\" 2>/dev/null; done");
}

static void
tweak_get_dirty_ratio (gboolean enable, char **out)
{
  *out = g_strdup (enable
    ? "sysctl vm.dirty_ratio=5 vm.dirty_background_ratio=3"
    : "sysctl vm.dirty_ratio=20 vm.dirty_background_ratio=10");
}

static void
tweak_get_kill_trackers (gboolean enable, char **out)
{
  (void) enable;
  if (enable)
    *out = g_strdup ("killall -9 tracker-store tracker-miner-fs baloo_file 2>/dev/null");
  else
    *out = g_strdup ("");
}

static void
tweak_get_sched_rt (gboolean enable, char **out)
{
  *out = g_strdup (enable
    ? "sysctl kernel.sched_rt_runtime_us=990000"
    : "sysctl kernel.sched_rt_runtime_us=950000");
}

static void
tweak_get_stop_services (gboolean enable, char **out)
{
  if (enable)
    *out = g_strdup ("systemctl stop packagekit-offline-update snapd 2>/dev/null");
  else
    *out = g_strdup ("");
}

/* ---- Tweak builder helpers ---- */

#define TWEAK_IS_ACTIVE(page, sw)  adw_switch_row_get_active ((page)->sw)

static void
tweak_build (GameModePage *page, gboolean enable, const char *user_script, char **out)
{
  GString *s = g_string_new ("");
  if (user_script && user_script[0])
    g_string_append (s, user_script);

  struct { gboolean active; void (*get)(gboolean e, char **o); } tw[] = {
    { TWEAK_IS_ACTIVE (page, tweak_disable_animations),    tweak_get_anim },
    { TWEAK_IS_ACTIVE (page, tweak_disable_notifications), tweak_get_dnd },
    { TWEAK_IS_ACTIVE (page, tweak_suspend_compositor),    tweak_get_compositor },
    { TWEAK_IS_ACTIVE (page, tweak_swappiness),            tweak_get_swappiness },
    { TWEAK_IS_ACTIVE (page, tweak_nmi_watchdog),          tweak_get_nmi_watchdog },
    { TWEAK_IS_ACTIVE (page, tweak_usb_autosuspend),       tweak_get_usb_autosuspend },
    { TWEAK_IS_ACTIVE (page, tweak_timer_khz),             tweak_get_timer_khz },
    { TWEAK_IS_ACTIVE (page, tweak_disable_wakeup),        tweak_get_disable_wakeup },
    { TWEAK_IS_ACTIVE (page, tweak_dirty_ratio),           tweak_get_dirty_ratio },
    { TWEAK_IS_ACTIVE (page, tweak_kill_trackers),        tweak_get_kill_trackers },
    { TWEAK_IS_ACTIVE (page, tweak_sched_rt),              tweak_get_sched_rt },
    { TWEAK_IS_ACTIVE (page, tweak_stop_services),          tweak_get_stop_services },
  };

  gboolean any = FALSE;
  for (size_t i = 0; i < G_N_ELEMENTS (tw); i++)
    if (tw[i].active) { any = TRUE; break; }

  if (!any)
    {
      *out = g_string_free (s, FALSE);
      return;
    }

  g_string_append (s, "\n");
  g_string_append (s, TWEAK_MARKER_START);

  for (size_t i = 0; i < G_N_ELEMENTS (tw); i++)
    if (tw[i].active)
      {
        char *cmd = NULL;
        tw[i].get (enable, &cmd);
        if (cmd && cmd[0])
          g_string_append_printf (s, "%s\n", cmd);
        g_free (cmd);
      }

  g_string_append (s, TWEAK_MARKER_END);
  *out = g_string_free (s, FALSE);
}

static void
tweak_build_custom_start (GameModePage *page, const char *user_script, char **out)
{
  tweak_build (page, FALSE, user_script, out);
}

static void
tweak_build_custom_end (GameModePage *page, const char *user_script, char **out)
{
  tweak_build (page, TRUE, user_script, out);
}

static char *
tweak_extract_user_script (const char *raw)
{
  if (!raw || !raw[0]) return g_strdup ("");

  const char *marker = strstr (raw, TWEAK_MARKER_START);
  if (!marker)
    return g_strstrip (g_strdup (raw));

  char *user_part = g_strndup (raw, marker - raw);
  return g_strstrip (user_part);
}

/* ---- Section toggle ---- */

static void
on_section_toggle (GtkButton *btn, GtkWidget *content)
{
  if (!content || !GTK_IS_WIDGET (content)) return;
  gboolean visible = gtk_widget_get_visible (content);
  gtk_widget_set_visible (content, !visible);

  GtkImage *arrow = GTK_IMAGE (g_object_get_data (G_OBJECT (btn), "arrow"));
  if (visible)
    {
      gtk_widget_remove_css_class (GTK_WIDGET (btn), "collapsed");
      gtk_image_set_from_icon_name (arrow, "go-down-symbolic");
    }
  else
    {
      gtk_widget_add_css_class (GTK_WIDGET (btn), "collapsed");
      gtk_image_set_from_icon_name (arrow, "go-next-symbolic");
    }
}

static void
connect_section (GtkBuilder *builder, const char *toggle_id,
                 const char *arrow_id, const char *content_id)
{
  GtkWidget *toggle = GTK_WIDGET (gtk_builder_get_object (builder, toggle_id));
  GtkWidget *arrow  = GTK_WIDGET (gtk_builder_get_object (builder, arrow_id));
  GtkWidget *content = GTK_WIDGET (gtk_builder_get_object (builder, content_id));

  g_object_set_data (G_OBJECT (toggle), "arrow", arrow);
  g_signal_connect (toggle, "clicked", G_CALLBACK (on_section_toggle), content);
}

/* ---- Save ---- */

static void
on_save_clicked (GtkButton *btn, GameModePage *page)
{
  (void) btn;

  const char *cfg = config_path ();
  char *dir = g_path_get_dirname (cfg);
  g_mkdir_with_parents (dir, 0755);
  g_free (dir);

  GString *content = g_string_new (NULL);

  g_string_append (content, "[general]\n");
  g_string_append_printf (content, "renice = %d\n", (int) gtk_spin_button_get_value (page->renice));
  g_string_append_printf (content, "ioprio = %d\n", (int) adw_combo_row_get_selected (page->ioprio));
  g_string_append_printf (content, "cpu_governor = %s\n",
    adw_combo_row_get_selected (page->cpu_governor) == 0 ? "performance" :
    adw_combo_row_get_selected (page->cpu_governor) == 1 ? "powersave" : "schedutil");
  g_string_append_printf (content, "cpu_min_freq = %d\n", (int) gtk_spin_button_get_value (page->cpu_min_freq));
  g_string_append_printf (content, "cpu_max_freq = %d\n", (int) gtk_spin_button_get_value (page->cpu_max_freq));
  g_string_append_printf (content, "cpu_perf_pct = %d\n", (int) gtk_spin_button_get_value (page->cpu_perf_pct));

  g_string_append (content, "\n[scheduler]\n");
  g_string_append_printf (content, "scheduler_priority = %d\n", (int) gtk_spin_button_get_value (page->scheduler_priority));
  g_string_append_printf (content, "io_weight = %d\n", (int) gtk_spin_button_get_value (page->io_weight));

  g_string_append (content, "\n[gpu]\n");
  g_string_append_printf (content, "gpu_governor = %s\n",
    adw_combo_row_get_selected (page->gpu_governor) == 0 ? "performance" :
    adw_combo_row_get_selected (page->gpu_governor) == 1 ? "powersave" : "default");
  g_string_append_printf (content, "apply_gpu_governor = %s\n",
    adw_switch_row_get_active (page->apply_gpu_governor) ? "1" : "0");

  g_string_append (content, "\n[tweaks]\n");
  g_string_append_printf (content, "disable_animations = %s\n",
    adw_switch_row_get_active (page->tweak_disable_animations) ? "true" : "false");
  g_string_append_printf (content, "disable_notifications = %s\n",
    adw_switch_row_get_active (page->tweak_disable_notifications) ? "true" : "false");
  g_string_append_printf (content, "suspend_compositor = %s\n",
    adw_switch_row_get_active (page->tweak_suspend_compositor) ? "true" : "false");
  g_string_append_printf (content, "swappiness = %s\n",
    adw_switch_row_get_active (page->tweak_swappiness) ? "true" : "false");
  g_string_append_printf (content, "nmi_watchdog = %s\n",
    adw_switch_row_get_active (page->tweak_nmi_watchdog) ? "true" : "false");
  g_string_append_printf (content, "usb_autosuspend = %s\n",
    adw_switch_row_get_active (page->tweak_usb_autosuspend) ? "true" : "false");
  g_string_append_printf (content, "timer_khz = %s\n",
    adw_switch_row_get_active (page->tweak_timer_khz) ? "true" : "false");
  g_string_append_printf (content, "disable_wakeup = %s\n",
    adw_switch_row_get_active (page->tweak_disable_wakeup) ? "true" : "false");
  g_string_append_printf (content, "dirty_ratio = %s\n",
    adw_switch_row_get_active (page->tweak_dirty_ratio) ? "true" : "false");
  g_string_append_printf (content, "kill_trackers = %s\n",
    adw_switch_row_get_active (page->tweak_kill_trackers) ? "true" : "false");
  g_string_append_printf (content, "sched_rt = %s\n",
    adw_switch_row_get_active (page->tweak_sched_rt) ? "true" : "false");
  g_string_append_printf (content, "stop_services = %s\n",
    adw_switch_row_get_active (page->tweak_stop_services) ? "true" : "false");

  const char *user_start = gtk_editable_get_text (GTK_EDITABLE (page->script_start));
  const char *user_end   = gtk_editable_get_text (GTK_EDITABLE (page->script_end));

  char *tweaked_start = NULL;
  char *tweaked_end   = NULL;
  tweak_build_custom_start (page, user_start, &tweaked_start);
  tweak_build_custom_end   (page, user_end,   &tweaked_end);

  gboolean has_start = (tweaked_start && tweaked_start[0]);
  gboolean has_end   = (tweaked_end   && tweaked_end[0]);

  if (has_start || has_end)
    {
      g_string_append (content, "\n[custom]\n");
      if (has_start)
        g_string_append_printf (content, "start = %s\n", tweaked_start);
      if (has_end)
        g_string_append_printf (content, "end = %s\n", tweaked_end);
    }

  g_free (tweaked_start);
  g_free (tweaked_end);

  GError *err = NULL;
  if (g_file_set_contents (config_path (), content->str, -1, &err))
    {
      dashboard_show_toast ("Configuration saved successfully");
    }
  else
    {
      g_clear_error (&err);

      /* Write to temp file first */
      char tmp_path[] = "/tmp/gamemode-conf-XXXXXX";
      int fd = g_mkstemp_full (tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
      if (fd < 0)
        {
          dashboard_show_toast ("Failed to create temp file");
          g_string_free (content, TRUE);
          return;
        }
      FILE *f = fdopen (fd, "w");
      if (f)
        {
          fputs (content->str, f);
          fclose (f);
        }
      else
        close (fd);

      const char *cfg_path = config_path ();
      char *install_cmd = g_strdup_printf (
        "install -D -m 644 '%s' '%s'", tmp_path, cfg_path);
      int exit_code = -1;
      GError *err2 = NULL;
      if (epi_privilege_exec_sync (install_cmd, &exit_code, NULL, &err2)
          && exit_code == 0)
        {
          dashboard_show_toast ("Saved with admin privileges");
          unlink (tmp_path);
        }
      else
        {
          dashboard_show_toast ("Failed to save (elevation denied)");
          unlink (tmp_path);
        }
      g_free (install_cmd);
    }

  g_string_free (content, TRUE);
}

/* ---- Load ---- */

static void
load_tweak_bool (AdwSwitchRow *sw, const char *value)
{
  adw_switch_row_set_active (sw, g_strcmp0 (value, "true") == 0);
}

static void
load_config (GameModePage *page)
{
  char *contents = NULL;
  GError *err = NULL;

  if (!g_file_get_contents (config_path (), &contents, NULL, &err))
    {
      g_error_free (err);
      return;
    }

  char **lines = g_strsplit (contents, "\n", -1);
  char *section = NULL;

  for (int i = 0; lines[i]; i++)
    {
      char *line = g_strstrip (lines[i]);

      if (line[0] == '[')
        {
          g_free (section);
          section = g_strdup (line);
          continue;
        }

      if (line[0] == '\0' || line[0] == ';')
        continue;

      char *eq = strchr (line, '=');
      if (!eq) continue;

      *eq = '\0';
      char *key = g_strstrip (line);
      char *value = g_strstrip (eq + 1);

      if (g_strcmp0 (section, "[general]") == 0)
        {
          if (g_strcmp0 (key, "renice") == 0)
            gtk_spin_button_set_value (page->renice, atoi (value));
          else if (g_strcmp0 (key, "ioprio") == 0)
            adw_combo_row_set_selected (page->ioprio, atoi (value));
          else if (g_strcmp0 (key, "cpu_governor") == 0)
            {
              if (g_strcmp0 (value, "performance") == 0)
                adw_combo_row_set_selected (page->cpu_governor, 0);
              else if (g_strcmp0 (value, "powersave") == 0)
                adw_combo_row_set_selected (page->cpu_governor, 1);
              else
                adw_combo_row_set_selected (page->cpu_governor, 2);
            }
          else if (g_strcmp0 (key, "cpu_min_freq") == 0)
            gtk_spin_button_set_value (page->cpu_min_freq, atoi (value));
          else if (g_strcmp0 (key, "cpu_max_freq") == 0)
            gtk_spin_button_set_value (page->cpu_max_freq, atoi (value));
          else if (g_strcmp0 (key, "cpu_perf_pct") == 0)
            gtk_spin_button_set_value (page->cpu_perf_pct, atoi (value));
        }
      else if (g_strcmp0 (section, "[scheduler]") == 0)
        {
          if (g_strcmp0 (key, "scheduler_priority") == 0)
            gtk_spin_button_set_value (page->scheduler_priority, atoi (value));
          else if (g_strcmp0 (key, "io_weight") == 0)
            gtk_spin_button_set_value (page->io_weight, atoi (value));
        }
      else if (g_strcmp0 (section, "[gpu]") == 0)
        {
          if (g_strcmp0 (key, "gpu_governor") == 0)
            {
              if (g_strcmp0 (value, "performance") == 0)
                adw_combo_row_set_selected (page->gpu_governor, 0);
              else if (g_strcmp0 (value, "powersave") == 0)
                adw_combo_row_set_selected (page->gpu_governor, 1);
              else
                adw_combo_row_set_selected (page->gpu_governor, 2);
            }
          else if (g_strcmp0 (key, "apply_gpu_governor") == 0)
            adw_switch_row_set_active (page->apply_gpu_governor,
              g_strcmp0 (value, "1") == 0);
        }
      else if (g_strcmp0 (section, "[tweaks]") == 0)
        {
          if (g_strcmp0 (key, "disable_animations") == 0)
            load_tweak_bool (page->tweak_disable_animations, value);
          else if (g_strcmp0 (key, "disable_notifications") == 0)
            load_tweak_bool (page->tweak_disable_notifications, value);
          else if (g_strcmp0 (key, "suspend_compositor") == 0)
            load_tweak_bool (page->tweak_suspend_compositor, value);
          else if (g_strcmp0 (key, "swappiness") == 0)
            load_tweak_bool (page->tweak_swappiness, value);
          else if (g_strcmp0 (key, "nmi_watchdog") == 0)
            load_tweak_bool (page->tweak_nmi_watchdog, value);
          else if (g_strcmp0 (key, "usb_autosuspend") == 0)
            load_tweak_bool (page->tweak_usb_autosuspend, value);
          else if (g_strcmp0 (key, "timer_khz") == 0)
            load_tweak_bool (page->tweak_timer_khz, value);
          else if (g_strcmp0 (key, "disable_wakeup") == 0)
            load_tweak_bool (page->tweak_disable_wakeup, value);
          else if (g_strcmp0 (key, "dirty_ratio") == 0)
            load_tweak_bool (page->tweak_dirty_ratio, value);
          else if (g_strcmp0 (key, "kill_trackers") == 0)
            load_tweak_bool (page->tweak_kill_trackers, value);
          else if (g_strcmp0 (key, "sched_rt") == 0)
            load_tweak_bool (page->tweak_sched_rt, value);
          else if (g_strcmp0 (key, "stop_services") == 0)
            load_tweak_bool (page->tweak_stop_services, value);
        }
      else if (g_strcmp0 (section, "[custom]") == 0)
        {
          if (g_strcmp0 (key, "start") == 0)
            {
              char *user = tweak_extract_user_script (value);
              gtk_editable_set_text (GTK_EDITABLE (page->script_start), user);
              g_free (user);
            }
          else if (g_strcmp0 (key, "end") == 0)
            {
              char *user = tweak_extract_user_script (value);
              gtk_editable_set_text (GTK_EDITABLE (page->script_end), user);
              g_free (user);
            }
        }
    }

  g_free (section);
  g_strfreev (lines);
  g_free (contents);
}

/* ---- Reset ---- */

static void
on_reset_clicked (GtkButton *btn, GameModePage *page)
{
  (void) btn;

  gtk_spin_button_set_value (page->renice, 0);
  adw_combo_row_set_selected (page->ioprio, 0);
  adw_combo_row_set_selected (page->cpu_governor, 0);
  gtk_spin_button_set_value (page->cpu_min_freq, 0);
  gtk_spin_button_set_value (page->cpu_max_freq, 0);
  gtk_spin_button_set_value (page->cpu_perf_pct, 100);
  gtk_spin_button_set_value (page->scheduler_priority, 0);
  gtk_spin_button_set_value (page->io_weight, 100);
  adw_combo_row_set_selected (page->gpu_governor, 0);
  adw_switch_row_set_active (page->apply_gpu_governor, FALSE);
  gtk_editable_set_text (GTK_EDITABLE (page->script_start), "");
  gtk_editable_set_text (GTK_EDITABLE (page->script_end), "");

  adw_switch_row_set_active (page->tweak_disable_animations, FALSE);
  adw_switch_row_set_active (page->tweak_disable_notifications, FALSE);
  adw_switch_row_set_active (page->tweak_suspend_compositor, FALSE);
  adw_switch_row_set_active (page->tweak_swappiness, FALSE);
  adw_switch_row_set_active (page->tweak_nmi_watchdog, FALSE);
  adw_switch_row_set_active (page->tweak_usb_autosuspend, FALSE);
  adw_switch_row_set_active (page->tweak_timer_khz, FALSE);
  adw_switch_row_set_active (page->tweak_disable_wakeup, FALSE);
  adw_switch_row_set_active (page->tweak_dirty_ratio, FALSE);
  adw_switch_row_set_active (page->tweak_kill_trackers, FALSE);
  adw_switch_row_set_active (page->tweak_sched_rt, FALSE);
  adw_switch_row_set_active (page->tweak_stop_services, FALSE);

  dashboard_show_toast ("Defaults restored (not saved)");
}

/* ---- Constructor ---- */

GameModePage *
gamemode_page_new (void)
{
  GameModePage *page = g_new0 (GameModePage, 1);

  GtkBuilder *builder = gtk_builder_new_from_resource (
    "/io/github/mayrinck/epinephrine/ui/page-gamemode.ui");

  page->box = GTK_WIDGET (gtk_builder_get_object (builder, "gamemode-page"));

  page->cpu_governor       = ADW_COMBO_ROW (gtk_builder_get_object (builder, "cpu-governor"));
  page->ioprio            = ADW_COMBO_ROW (gtk_builder_get_object (builder, "ioprio"));
  page->gpu_governor      = ADW_COMBO_ROW (gtk_builder_get_object (builder, "gpu-governor"));
  page->apply_gpu_governor = ADW_SWITCH_ROW (gtk_builder_get_object (builder, "apply-gpu-governor"));

  page->renice            = GTK_SPIN_BUTTON (gtk_builder_get_object (builder, "renice"));
  page->cpu_min_freq      = GTK_SPIN_BUTTON (gtk_builder_get_object (builder, "cpu-min-freq"));
  page->cpu_max_freq      = GTK_SPIN_BUTTON (gtk_builder_get_object (builder, "cpu-max-freq"));
  page->cpu_perf_pct      = GTK_SPIN_BUTTON (gtk_builder_get_object (builder, "cpu-perf-pct"));
  page->scheduler_priority = GTK_SPIN_BUTTON (gtk_builder_get_object (builder, "scheduler-priority"));
  page->io_weight         = GTK_SPIN_BUTTON (gtk_builder_get_object (builder, "io-weight"));

  page->script_start      = GTK_ENTRY (gtk_builder_get_object (builder, "script-start"));
  page->script_end        = GTK_ENTRY (gtk_builder_get_object (builder, "script-end"));

  page->tweak_disable_animations    = ADW_SWITCH_ROW (gtk_builder_get_object (builder, "tweak-disable-animations"));
  page->tweak_disable_notifications = ADW_SWITCH_ROW (gtk_builder_get_object (builder, "tweak-disable-notifications"));
  page->tweak_suspend_compositor    = ADW_SWITCH_ROW (gtk_builder_get_object (builder, "tweak-suspend-compositor"));
  page->tweak_swappiness            = ADW_SWITCH_ROW (gtk_builder_get_object (builder, "tweak-swappiness"));
  page->tweak_nmi_watchdog          = ADW_SWITCH_ROW (gtk_builder_get_object (builder, "tweak-nmi-watchdog"));
  page->tweak_usb_autosuspend       = ADW_SWITCH_ROW (gtk_builder_get_object (builder, "tweak-usb-autosuspend"));
  page->tweak_timer_khz             = ADW_SWITCH_ROW (gtk_builder_get_object (builder, "tweak-timer-khz"));
  page->tweak_disable_wakeup        = ADW_SWITCH_ROW (gtk_builder_get_object (builder, "tweak-disable-wakeup"));
  page->tweak_dirty_ratio           = ADW_SWITCH_ROW (gtk_builder_get_object (builder, "tweak-dirty-ratio"));
  page->tweak_kill_trackers         = ADW_SWITCH_ROW (gtk_builder_get_object (builder, "tweak-kill-trackers"));
  page->tweak_sched_rt              = ADW_SWITCH_ROW (gtk_builder_get_object (builder, "tweak-sched-rt"));
  page->tweak_stop_services        = ADW_SWITCH_ROW (gtk_builder_get_object (builder, "tweak-stop-services"));

  page->save_config_button = GTK_BUTTON (gtk_builder_get_object (builder, "save-config-button"));
  page->reset_config_button = GTK_BUTTON (gtk_builder_get_object (builder, "reset-config-button"));

  g_signal_connect (page->save_config_button, "clicked",
    G_CALLBACK (on_save_clicked), page);
  g_signal_connect (page->reset_config_button, "clicked",
    G_CALLBACK (on_reset_clicked), page);

  connect_section (builder, "gm-section-priority-toggle",  "gm-section-priority-arrow",  "gm-section-priority-content");
  connect_section (builder, "gm-section-cpu-toggle",      "gm-section-cpu-arrow",      "gm-section-cpu-content");
  connect_section (builder, "gm-section-scheduler-toggle",  "gm-section-scheduler-arrow", "gm-section-scheduler-content");
  connect_section (builder, "gm-section-gpu-toggle",      "gm-section-gpu-arrow",      "gm-section-gpu-content");
  connect_section (builder, "gm-section-scripts-toggle",  "gm-section-scripts-arrow",  "gm-section-scripts-content");
  connect_section (builder, "gm-section-tweaks-toggle",  "gm-section-tweaks-arrow",   "gm-section-tweaks-content");

  load_config (page);

  g_object_ref_sink (page->box);
  g_object_unref (builder);

  return page;
}