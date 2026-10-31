#include <adwaita.h>
#include <glib/gstdio.h>
#include "pages/page-games.h"
#include "pages/game-artwork.h"
#include "epi-i18n.h"
static void spin(int ms) {gint64 end=g_get_monotonic_time()+ms*1000;while(g_get_monotonic_time()<end){while(g_main_context_iteration(NULL,FALSE));g_usleep(1000);}}
static GtkWidget *find_check(GtkWidget *w){if(GTK_IS_CHECK_BUTTON(w))return w;for(GtkWidget *c=gtk_widget_get_first_child(w);c;c=gtk_widget_get_next_sibling(c)){GtkWidget *found=find_check(c);if(found)return found;}return NULL;}
static int buttons(GtkWidget *w) {int n=GTK_IS_BUTTON(w);for(GtkWidget *c=gtk_widget_get_first_child(w);c;c=gtk_widget_get_next_sibling(c))n+=buttons(c);return n;}
int main(int argc,char **argv) {
 if(argc>2 && g_strcmp0(argv[1],"-c")==0){g_print("[game]\npath=\n");return 0;}
 g_autofree char *tmp=g_dir_make_tmp("epi-ui-XXXXXX",NULL);
 g_setenv("XDG_CONFIG_HOME",tmp,TRUE);g_setenv("XDG_CACHE_HOME",tmp,TRUE);g_setenv("XDG_DATA_HOME",tmp,TRUE);
 g_autofree char *bin=g_build_filename(tmp,"bin",NULL);g_mkdir(bin,0700);
 g_autofree char *python=g_build_filename(bin,"python3",NULL),*self=g_file_read_link("/proc/self/exe",NULL);
 GFile *link=g_file_new_for_path(python);g_file_make_symbolic_link(link,self,NULL,NULL);g_object_unref(link);
 g_autofree char *path=g_strdup_printf("%s:%s",bin,g_getenv("PATH"));g_setenv("PATH",path,TRUE);
 if(!gtk_init_check()) return 77;
 adw_init();epi_i18n_set_language("en_US");
 DashboardPage page={0};page.running_game_idx=-1;page.n_games=3000;page.games=g_new0(GameShortcut,page.n_games);page.selected=g_new0(gboolean,page.n_games);
 for(int i=0;i<page.n_games;i++){page.games[i].name=g_strdup_printf("Game %04d",i);page.games[i].command=g_strdup_printf("game-%d",i);page.games[i].source=g_strdup("local");}
 GtkWidget *library=games_page_new(&page);page.box=library;refresh_games_grid(&page);
 GtkWidget *win=adw_window_new();adw_window_set_content(ADW_WINDOW(win),library);gtk_window_set_default_size(GTK_WINDOW(win),1000,700);gtk_window_present(GTK_WINDOW(win));spin(600);
 g_assert_true(GTK_IS_GRID_VIEW(page.library_cover_grid));g_assert_true(GTK_IS_LIST_VIEW(page.games_grid));
 g_assert_cmpint(g_list_model_get_n_items(G_LIST_MODEL(page.library_model)),==,3000);
 g_assert_cmpint(buttons(page.library_cover_grid),<,1000);
 games_page_show_game(&page,2000);g_assert_cmpstr(page.focused_game_command,==,"game-2000");
 page.library_grid_mode=FALSE;refresh_games_grid(&page);spin(300);g_assert_cmpint(buttons(page.games_grid),<,1000);
 GtkWidget *check=NULL;
 for(int retry=0;retry<50&&!check;retry++){spin(100);check=find_check(page.games_grid);}
 g_assert_nonnull(check);gtk_check_button_set_active(GTK_CHECK_BUTTON(check),TRUE);spin(100);
 save_games_config(&page);g_autofree char *id=g_strdup(page.games[0].id);g_assert_true(g_uuid_string_is_valid(id));
 load_games_config(&page);g_assert_cmpstr(page.games[0].id,==,id);refresh_games_grid(&page);
 g_assert_true(game_artwork_move_identity(page.games[0].command,"changed-command"));
 g_autofree char *resolved=game_artwork_identity("changed-command");g_assert_cmpstr(id,==,resolved);
 gtk_window_destroy(GTK_WINDOW(win));g_object_unref(library);spin(200);
 for(int i=0;i<page.n_games;i++){
  g_free(page.games[i].id);g_free(page.games[i].name);g_free(page.games[i].command);
  g_free(page.games[i].source);g_free(page.games[i].icon);g_free(page.games[i].workdir);
  g_free(page.games[i].startup_cmd);g_free(page.games[i].group);
 }
 g_free(page.games);g_free(page.selected);g_free(page.focused_game_command);
 g_free(page.active_group);g_free(page.active_service);

 g_print("Virtualized 3000 games and verified stable ID migration.\n");return 0;
}
