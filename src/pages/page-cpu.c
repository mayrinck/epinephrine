#include "page-cpu.h"
#include "page-dashboard.h"

#include <string.h>

static void
on_cpu_save (GtkButton *btn, CpuPage *page)
{
  (void) btn;

  const char *governor = NULL;
  {
    guint sel = gtk_drop_down_get_selected (GTK_DROP_DOWN (page->governor_dropdown));
    static const char *govs[] = { "performance", "powersave", "ondemand", "conservative", "schedutil", "userspace" };
    if (sel < G_N_ELEMENTS (govs))
      governor = govs[sel];
  }

  if (governor && page->n_cores > 0)
    {
      for (int i = 0; i < page->n_cores; i++)
        {
          char *path = g_strdup_printf ("/sys/devices/system/cpu/cpu%d/cpufreq/scaling_governor", i);
          FILE *f = fopen (path, "w");
          if (f)
            {
              fprintf (f, "%s", governor);
              fclose (f);
            }
          g_free (path);
        }
    }

  int min_freq = (int) gtk_spin_button_get_value (GTK_SPIN_BUTTON (page->min_freq_spin));
  int max_freq = (int) gtk_spin_button_get_value (GTK_SPIN_BUTTON (page->max_freq_spin));

  if (min_freq > 0 && page->n_cores > 0)
    {
      for (int i = 0; i < page->n_cores; i++)
        {
          char *path = g_strdup_printf ("/sys/devices/system/cpu/cpu%d/cpufreq/scaling_min_freq", i);
          FILE *f = fopen (path, "w");
          if (f) { fprintf (f, "%d", min_freq); fclose (f); }
          g_free (path);
        }
    }

  if (max_freq > 0 && page->n_cores > 0)
    {
      for (int i = 0; i < page->n_cores; i++)
        {
          char *path = g_strdup_printf ("/sys/devices/system/cpu/cpu%d/cpufreq/scaling_max_freq", i);
          FILE *f = fopen (path, "w");
          if (f) { fprintf (f, "%d", max_freq); fclose (f); }
          g_free (path);
        }
    }

  dashboard_show_toast ("CPU settings applied");
}

static void
refresh_cpu_info (CpuPage *page)
{
  FILE *f;
  char buf[256];

  /* Read current governor from cpu0 */
  f = fopen ("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor", "r");
  if (f)
    {
      if (fgets (buf, sizeof (buf), f))
        {
          g_strstrip (buf);
          gtk_label_set_text (GTK_LABEL (page->governor_current_label), buf);
        }
      fclose (f);
    }

  /* Read current frequency */
  f = fopen ("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", "r");
  if (f)
    {
      if (fgets (buf, sizeof (buf), f))
        {
          g_strstrip (buf);
          long khz = atol (buf);
          char freq_str[64];
          if (khz >= 1000000)
            g_snprintf (freq_str, sizeof (freq_str), "%.2f GHz", khz / 1000000.0);
          else
            g_snprintf (freq_str, sizeof (freq_str), "%.0f MHz", khz / 1000.0);
          gtk_label_set_text (GTK_LABEL (page->cur_freq_label), freq_str);
        }
      fclose (f);
    }

  /* Read cores count */
  page->n_cores = 0;
  f = fopen ("/proc/cpuinfo", "r");
  if (f)
    {
      while (fgets (buf, sizeof (buf), f))
        if (g_str_has_prefix (buf, "processor"))
          page->n_cores++;
      fclose (f);
    }

  char cores_str[32];
  g_snprintf (cores_str, sizeof (cores_str), "%d", page->n_cores);
  gtk_label_set_text (GTK_LABEL (page->cpu_cores_label), cores_str);

  /* Architecture */
  f = fopen ("/proc/sys/kernel/arch", "r");
  if (!f)
    f = fopen ("/proc/version", "r");
  if (f)
    {
      if (fgets (buf, sizeof (buf), f))
        {
          g_strstrip (buf);
          /* Extract arch from /proc/version:
             "Linux version 6.x.x-arch1-1 (linux@archlinux) (gcc ...) #1 SMP PREEMPT_DYNAMIC ..." */
          char *u = strstr (buf, "(gcc");
          if (u)
            {
              while (u > buf && *u != ' ') u--;
              if (u > buf) *u = '\0';
            }
          gtk_label_set_text (GTK_LABEL (page->cpu_arch_label), buf);
        }
      fclose (f);
    }
}

static void
on_cpu_openrgb_clicked (GtkButton *btn, CpuPage *page)
{
  (void) btn;
  (void) page;
  GError *err = NULL;
  g_spawn_async (NULL, (char *[]){ "openrgb", NULL },
    NULL, G_SPAWN_DEFAULT | G_SPAWN_SEARCH_PATH,
    NULL, NULL, NULL, &err);
  if (err)
    {
      dashboard_show_toast ("OpenRGB not installed. Install it from openrgb.org");
      g_error_free (err);
    }
}

CpuPage *
cpu_page_new (void)
{
  CpuPage *page = g_new0 (CpuPage, 1);

  GtkBuilder *builder = gtk_builder_new_from_resource (
    "/io/github/mayrinck/epinephrine/ui/page-cpu.ui");

  page->box = GTK_WIDGET (gtk_builder_get_object (builder, "cpu-page"));
  page->governor_dropdown = GTK_WIDGET (gtk_builder_get_object (builder, "governor-dropdown"));
  page->governor_current_label = GTK_WIDGET (gtk_builder_get_object (builder, "governor-current-label"));
  page->min_freq_spin = GTK_WIDGET (gtk_builder_get_object (builder, "min-freq-spin"));
  page->max_freq_spin = GTK_WIDGET (gtk_builder_get_object (builder, "max-freq-spin"));
  page->cur_freq_label = GTK_WIDGET (gtk_builder_get_object (builder, "cur-freq-label"));
  page->cpu_cores_label = GTK_WIDGET (gtk_builder_get_object (builder, "cpu-cores-label"));
  page->cpu_arch_label = GTK_WIDGET (gtk_builder_get_object (builder, "cpu-arch-label"));
  page->save_btn = GTK_WIDGET (gtk_builder_get_object (builder, "cpu-save-btn"));

  /* Lighting Controls */
  page->lighting_status = GTK_WIDGET (gtk_builder_get_object (builder, "cpu-lighting-status"));
  gtk_label_set_text (GTK_LABEL (page->lighting_status),
    g_dgettext (GETTEXT_PACKAGE, "The app couldn't find any light source to manage."));

  GtkWidget *cpu_lighting_header = GTK_WIDGET (gtk_builder_get_object (builder, "cpu-lighting-header"));
  if (g_find_program_in_path ("openrgb"))
    {
      GtkWidget *openrgb_btn = gtk_button_new_with_label (
        g_dgettext (GETTEXT_PACKAGE, "Edit on OpenRGB"));
      gtk_widget_add_css_class (openrgb_btn, "accent");
      gtk_widget_set_valign (openrgb_btn, GTK_ALIGN_CENTER);
      gtk_box_append (GTK_BOX (cpu_lighting_header), openrgb_btn);
      g_signal_connect (openrgb_btn, "clicked", G_CALLBACK (on_cpu_openrgb_clicked), page);
    }

  g_signal_connect (page->save_btn, "clicked", G_CALLBACK (on_cpu_save), page);

  g_object_ref_sink (page->box);

  refresh_cpu_info (page);

  return page;
}