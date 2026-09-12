#define _GNU_SOURCE
#include <linux/input.h>
#include "page-hardware.h"
#include "hw-device-list.h"
#include "hw-devices.h"
#include "hw-gpu.h"
#include "hw-detail.h"
#include "hw-gpu-detail.h"
#include "hw-controller.h"
#include "hw-monitor.h"
#include "../epi-i18n.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define tr(x) epi_i18n_get (x)

void
hw_device_list_populate (HardwarePage *page)
{
  for (int i = 0; i < page->n_devices; i++)
    {
      const char *icon_name = NULL;
      const char *type = page->device_types[i];
      if (g_strcmp0 (type, "motherboard") == 0)
        icon_name = "motherboard-symbolic";
      else if (g_strcmp0 (type, "cpu") == 0)
        icon_name = "cpu-symbolic";
      else if (g_strcmp0 (type, "gpu") == 0)
        icon_name = "pci-card-symbolic";
      else if (g_strcmp0 (type, "ram") == 0)
        icon_name = "memory-symbolic";
      else if (g_strcmp0 (type, "keyboard") == 0)
        icon_name = "keyboard-symbolic";
      else if (g_strcmp0 (type, "mouse") == 0)
        icon_name = "mouse-symbolic";
      else if (g_strcmp0 (type, "joystick") == 0)
        icon_name = "joystick-symbolic";
      else if (g_strcmp0 (type, "controller") == 0)
        icon_name = "gamepad-symbolic";
      else if (g_strcmp0 (type, "mic") == 0 || g_strcmp0 (type, "microphone") == 0)
        icon_name = "mic-symbolic";
      else if (g_strcmp0 (type, "headset") == 0 || g_strcmp0 (type, "headphone") == 0)
        icon_name = "headset-symbolic";
      else
        icon_name = "chip-symbolic";

      GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 10);
      gtk_widget_set_margin_start (row, 4);
      gtk_widget_set_margin_end (row, 4);
      gtk_widget_set_margin_top (row, 6);
      gtk_widget_set_margin_bottom (row, 6);

      if (icon_name)
        {
          GtkWidget *ico = gtk_image_new_from_icon_name (icon_name);
          gtk_image_set_pixel_size (GTK_IMAGE (ico), 20);
          gtk_widget_set_opacity (ico, 0.7);
          gtk_box_append (GTK_BOX (row), ico);
        }

      GtkWidget *label_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
      GtkWidget *title = gtk_label_new (page->device_names[i]);
      gtk_widget_set_halign (title, GTK_ALIGN_START);
      gtk_widget_add_css_class (title, "body");
      gtk_label_set_ellipsize (GTK_LABEL (title), PANGO_ELLIPSIZE_END);
      gtk_box_append (GTK_BOX (label_box), title);

      if (page->device_info[i] && page->device_info[i][0])
        {
          GtkWidget *sub = gtk_label_new (page->device_info[i]);
          gtk_widget_set_halign (sub, GTK_ALIGN_START);
          gtk_widget_add_css_class (sub, "dim-label");
          gtk_label_set_ellipsize (GTK_LABEL (sub), PANGO_ELLIPSIZE_END);
          gtk_box_append (GTK_BOX (label_box), sub);
        }

      gtk_box_append (GTK_BOX (row), label_box);
      gtk_list_box_insert (GTK_LIST_BOX (page->device_list), row, -1);
    }
}

static void
clear_device_list (HardwarePage *page)
{
  hw_controller_close (page);
  gtk_list_box_remove_all (GTK_LIST_BOX (page->device_list));
  for (int i = 0; i < page->n_devices; i++)
    {
      g_free (page->device_names[i]);
      g_free (page->device_types[i]);
      g_free (page->device_info[i]);
      g_free (page->device_paths[i]);
      g_free (page->device_urls[i]);
      g_free (page->device_categories[i]);
    }
  page->n_devices = 0;
  g_free (page->device_names);  page->device_names = NULL;
  g_free (page->device_types);  page->device_types = NULL;
  g_free (page->device_info);   page->device_info = NULL;
  g_free (page->device_paths);  page->device_paths = NULL;
  g_free (page->device_vendor); page->device_vendor = NULL;
  g_free (page->device_product); page->device_product = NULL;
  g_free (page->device_urls);   page->device_urls = NULL;
  g_free (page->device_categories); page->device_categories = NULL;
  g_free (page->controller_url); page->controller_url = NULL;
  g_free (page->mobo_vendor); page->mobo_vendor = NULL;
  g_free (page->mobo_model);  page->mobo_model = NULL;
  g_free (page->mobo_bios_version); page->mobo_bios_version = NULL;
  g_free (page->mobo_bios_vendor); page->mobo_bios_vendor = NULL;
  g_free (page->mobo_revision); page->mobo_revision = NULL;
  g_free (page->mobo_bios_date); page->mobo_bios_date = NULL;
  g_free (page->mobo_bios_romsize); page->mobo_bios_romsize = NULL;
  g_free (page->mobo_bios_efi_pk); page->mobo_bios_efi_pk = NULL;
  g_free (page->mobo_chipset_vendor); page->mobo_chipset_vendor = NULL;
  g_free (page->mobo_chipset_model); page->mobo_chipset_model = NULL;
  page->mobo_detected = FALSE;

  if (page->ram_sticks)
    {
      g_ptr_array_unref (page->ram_sticks);
      page->ram_sticks = NULL;
    }
  page->ram_slots_total = 0;
  page->ram_slots_used = 0;
  g_free (page->ram_technology); page->ram_technology = NULL;
  page->ram_freq_mhz = 0;
  page->ram_total_bytes = 0;
  page->ram_used_bytes = 0;
  page->ram_monitor_history_count = 0;
  memset (page->ram_monitor_history, 0, sizeof (page->ram_monitor_history));
  for (int i = 0; i < RAM_MON_METRICS; i++)
    page->ram_monitor_avail[i] = FALSE;
  hw_ram_monitor_stop (page);

  if (page->monitor_data_timer_id > 0)
    {
      g_source_remove (page->monitor_data_timer_id);
      page->monitor_data_timer_id = 0;
    }

  if (page->monitor_timer_id > 0)
    {
      g_source_remove (page->monitor_timer_id);
      page->monitor_timer_id = 0;
    }

  if (page->gpu)
    {
      g_free (page->gpu->vendor);
      g_free (page->gpu->device_name);
      g_free (page->gpu->kernel_driver);
      g_free (page->gpu->driver_name);
      g_free (page->gpu->driver_info);
      g_free (page->gpu->vulkan_version);
      g_free (page->gpu->sysfs_path);
      g_free (page->gpu->hwmon_path);
      g_free (page->gpu->hwmon_temp_file);
      g_free (page->gpu->gpu_specs);
      g_free (page->gpu->gpu_specs_url);
      g_free (page->gpu->gpu_release_date);
      g_free (page->gpu);
      page->gpu = NULL;
    }
  if (page->power_profile_names)
    {
      g_strfreev (page->power_profile_names);
      page->power_profile_names = NULL;
    }
  page->n_power_profiles = 0;
}

static gboolean
reload_devices_done (gpointer user_data)
{
  HardwarePage *page = user_data;
  hw_devices_detect (page);
  hw_gpu_populate_specs (page);
  hw_device_list_populate (page);
  gtk_widget_set_sensitive (page->reload_btn, TRUE);
  GtkListBoxRow *first = gtk_list_box_get_row_at_index (
    GTK_LIST_BOX (page->device_list), 0);
  if (first)
    gtk_list_box_select_row (GTK_LIST_BOX (page->device_list), first);
  else
    gtk_stack_set_visible_child_name (page->detail_stack, "default");
  return G_SOURCE_REMOVE;
}

void
hw_device_list_on_reload (GtkButton *btn, gpointer user_data)
{
  (void) btn;
  HardwarePage *page = user_data;
  gtk_widget_set_sensitive (page->reload_btn, FALSE);
  gtk_stack_set_visible_child_name (page->detail_stack, "loading");
  clear_device_list (page);
  g_timeout_add (1000, reload_devices_done, page);
}

void
hw_device_list_on_selected (GtkListBox *box, GtkListBoxRow *row, gpointer user_data)
{
  (void) box;
  HardwarePage *page = user_data;
  if (!row) return;
  int idx = gtk_list_box_row_get_index (row);
  if (idx < 0 || idx >= page->n_devices) return;

  hw_controller_close (page);

  /* Stop any active monitors */
  if (page->monitor_data_timer_id > 0)
    {
      g_source_remove (page->monitor_data_timer_id);
      page->monitor_data_timer_id = 0;
    }
  if (page->monitor_timer_id > 0)
    {
      g_source_remove (page->monitor_timer_id);
      page->monitor_timer_id = 0;
    }
  hw_ram_monitor_stop (page);

  const char *type = page->device_types[idx];
  const char *path = page->device_paths[idx];

  if (g_strcmp0 (type, "motherboard") == 0 && page->mobo_detected
      && page->mobo_vendor && page->mobo_model)
    {
      hw_detail_show_motherboard (page);
    }
  else if (g_strcmp0 (type, "ram") == 0)
    {
      hw_detail_show_ram (page, page->device_names[idx]);
    }
  else if (g_strcmp0 (type, "gpu") == 0 && page->gpu)
    {
      hw_gpu_detail_show (page);
    }
  else if ((g_strcmp0 (type, "controller") == 0 || g_strcmp0 (type, "joystick") == 0) && path && path[0])
    {
      hw_controller_show (page);
    }
  else if (g_strcmp0 (type, "cpu") == 0 && page->cpu_page)
    gtk_stack_set_visible_child_name (page->detail_stack, "cpu");
  else if (g_strcmp0 (type, "keyboard") == 0 && page->keyboard_page)
    {
      keyboard_page_show_device (page->keyboard_page,
        page->device_vendor[idx], page->device_product[idx]);
      gtk_stack_set_visible_child_name (page->detail_stack, "keyboard");
    }
  else if (g_strcmp0 (type, "mouse") == 0 && page->mouse_page)
    {
      mouse_page_show_device (page->mouse_page,
        page->device_vendor[idx], page->device_product[idx]);
      gtk_stack_set_visible_child_name (page->detail_stack, "mouse");
    }
  else
    gtk_stack_set_visible_child_name (page->detail_stack, "default");
}
