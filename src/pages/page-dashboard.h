#pragma once

#include <adwaita.h>

G_BEGIN_DECLS

#define DASHBOARD_N_RESOURCES 8
#define DASHBOARD_N_RECENT 5

typedef struct
{
  char *name;
  char *command;
  char *icon;
  char *workdir;
  char *source;   /* "steam", "heroic", "lutris", "bottles", "other", "local" */
  char *startup_cmd;
  char *group;    /* category name; NULL = ungrouped */
  gboolean feral_gamemode;
  gboolean mangohud;
  int play_time_secs;
  float avg_fps;
  gint64 last_played;
} GameShortcut;

typedef struct
{
  GtkWidget *box;
  GtkWidget *spinner;
  GtkWidget *scroll;
  GtkFlowBox *flowbox;

  GtkWidget *recent_box;

  GtkWidget *games_grid;
  GtkStack *games_stack;

  AdwActionRow *cpu_row;
  AdwActionRow *ram_row;
  AdwActionRow *gpu_row;
  AdwActionRow *vram_row;
  AdwActionRow *vulkan_row;
  AdwActionRow *driver_row;
  GtkWidget *hw_box;
  AdwActionRow *storage_rows[8];
  int n_storage_rows;

  struct
  {
    AdwActionRow *row;
    GtkWidget *prefix_icon;
    GtkWidget *status_icon;
    GtkWidget *installed_label;
    GtkWidget *install_btn;
    GtkProgressBar *progress;
    gboolean installed;
  } resources[DASHBOARD_N_RESOURCES];

  GameShortcut *games;
  int n_games;

  /* Game library edit / category state */
  gboolean edit_mode;
  gboolean *selected;      /* parallel to games[], NULL unless edit_mode */
  char *active_group;      /* NULL = "All" */
  char *active_service;    /* NULL = "All Services"; matches games[].source */
  GtkWidget *edit_btn;
  GtkWidget *category_bar; /* horizontal box of category chips */
  GtkWidget *service_dropdown; /* filter dropdown of sources in use */
  GtkWidget *edit_actions; /* box with Delete / Group / Ungroup while in edit */

  int running_game_idx;
  GPid game_pid;
  guint poll_timer_id;
  gint64 game_start_us;

  gboolean is_steam_game;
  guint steam_watch_id;
  int steam_watch_idle_secs;

  GtkWidget *gaming_score_box;
  GtkWidget *gaming_score_label;
  GtkWidget *gaming_score_tier;
  GtkWidget *gaming_score_spinner;
  GtkWidget *gaming_score_details_btn;
  GtkWidget *gaming_score_details;
  GtkWidget *gaming_score_footer;

  /* Per-component score bars (details view) */
  GtkWidget *gaming_score_cpu_bar;
  GtkWidget *gaming_score_gpu_bar;
  GtkWidget *gaming_score_ram_bar;
  GtkWidget *gaming_score_monitor_bar;
} DashboardPage;

DashboardPage *dashboard_page_new (void);

void dashboard_set_toast_overlay (DashboardPage *page, AdwToastOverlay *overlay);

int import_steam_games (DashboardPage *page);
int import_heroic_games (DashboardPage *page);
int import_lutris_games (DashboardPage *page);
int import_bottles_games (DashboardPage *page);
int import_hydra_games (DashboardPage *page);
int import_hydra_classics_games (DashboardPage *page);
int import_desktop_games (DashboardPage *page);
void save_games_config (DashboardPage *page);
void refresh_recent_games (DashboardPage *page);
void open_edit_dialog (DashboardPage *page, int idx);
void on_game_launch_clicked (GtkButton *btn, DashboardPage *page);
void dashboard_show_toast (const char *message);
char *format_time_ago (gint64 last_played);
const char *detect_source_from_command (const char *cmd);
gboolean gamemode_available (void);
gboolean mangohud_available (void);
GtkWidget *create_game_icon_widget (const char *icon, int pixel_size);

G_END_DECLS
