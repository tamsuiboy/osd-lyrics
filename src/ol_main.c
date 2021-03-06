/* -*- mode: C; c-basic-offset: 2; indent-tabs-mode: nil; -*- */
/*
 * Copyright (C) 2009-2011  Tiger Soldier
 *
 * This file is part of OSD Lyrics.
 * OSD Lyrics is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * OSD Lyrics is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with OSD Lyrics.  If not, see <http://www.gnu.org/licenses/>. 
 */
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <signal.h>
#include <pwd.h>
#include "config.h"
#include "ol_lrc.h"
#include "ol_player.h"
#include "ol_utils.h"
#include "ol_lrc_fetch.h"
#include "ol_lrc_fetch_ui.h"
#include "ol_trayicon.h"
#include "ol_intl.h"
#include "ol_config.h"
#include "ol_display_module.h"
#include "ol_keybindings.h"
#include "ol_lrc_fetch_module.h"
#include "ol_lyric_manage.h"
#include "ol_stock.h"
#include "ol_app.h"
#include "ol_notify.h"
#include "ol_lrclib.h"
#include "ol_debug.h"
#include "ol_singleton.h"

#define REFRESH_INTERVAL 100
#define INFO_INTERVAL 500
#define LRCDB_FILENAME "lrc.db"

gboolean _arg_debug_cb (const gchar *option_name,
                        const gchar *value,
                        gpointer data,
                        GError **error);
static gboolean _arg_version;

static GOptionEntry cmdargs[] =
{
  { "debug", 'd', G_OPTION_FLAG_OPTIONAL_ARG, G_OPTION_ARG_CALLBACK, _arg_debug_cb,
    N_ ("The level of debug messages to log, can be 'none', 'error', 'debug', or 'info'"), "level" },
  { "version", 'v', 0, G_OPTION_ARG_NONE, &_arg_version,
    N_ ("Shows the version of " PACKAGE_NAME), NULL},
  { NULL }
};

static gboolean first_run = TRUE;
static guint refresh_source = 0;
static guint info_timer = 0;
static struct OlPlayer *player = NULL;
static OlMusicInfo music_info = {0};
static gchar *previous_title = NULL;
static gchar *previous_artist = NULL;
static gchar *previous_uri = NULL;
static enum OlPlayerStatus previous_status = OL_PLAYER_UNKNOWN;
static gint previous_duration = 0;
static gint previous_position = -1;
static struct OlLrc *lrc_file = NULL;
static char *display_mode = NULL;
static struct OlDisplayModule *module = NULL;

static void _initialize (int argc, char **argv);
static gint _refresh_music_info (gpointer data);
static gint _refresh_player_info (gpointer data);
static void _check_music_change ();
static void _on_music_changed (void);
static gboolean _check_lyric_file (void);
static void _update_player_status (enum OlPlayerStatus status);
static gboolean _get_active_player (void);
static void _search_callback (struct OlLrcFetchResult *result,
                            void *userdata);
static void _download_callback (struct OlLrcDownloadResult *result);
static void _on_config_changed (OlConfig *config,
                                gchar *group,
                                gchar *name,
                                gpointer userdata);

static void
_on_config_changed (OlConfig *config,
                    gchar *group,
                    gchar *name,
                    gpointer userdata)
{
  if (strcmp (name, "display-mode") == 0)
  {
    char *mode = ol_config_get_string (config, group, name);
    if (display_mode == NULL ||
        ol_stricmp (mode, display_mode, -1) != 0)
    {
      if (display_mode != NULL)
        g_free (display_mode);
      display_mode = g_strdup (mode);
      ol_display_module_free (module);
      module = ol_display_module_new (display_mode);
      ol_display_module_set_music_info (module, &music_info);
      ol_display_module_set_duration (module, previous_duration);
      ol_display_module_set_lrc (module, lrc_file);
      ol_display_module_set_player (module, player);
      ol_display_module_set_status (module, previous_status);
    }
    g_free (mode);
  }
}

static void
_download_callback (struct OlLrcDownloadResult *result)
{
  ol_log_func ();
  if (result->filepath != NULL)
    ol_app_assign_lrcfile (result->info, result->filepath, TRUE);
  else
    ol_display_module_download_fail_message (module, _("Download failed"));
}

static void
_search_callback (struct OlLrcFetchResult *result,
                  void *userdata)
{
  ol_log_func ();
  ol_assert (result != NULL);
  ol_assert (result->engine != NULL);
  if (result->count > 0 && result->candidates != 0)
  {
    char *filename = ol_lyric_download_path (&result->info);
    if (filename == NULL)
    {
      ol_display_module_download_fail_message (module, _("Cannot create the lyric directory"));
    }
    else
    {
      if (module != NULL) {
        ol_display_module_clear_message (module);
	ol_display_module_clear_message (module);
      }
      ol_lrc_fetch_ui_show (result->engine, result->candidates, result->count,
                            &result->info,
                            filename);
      g_free (filename);
    }
  }
  else
  {
    if (module != NULL)
      ol_display_module_search_fail_message (module, _("Lyrics not found"));
  }
}

gboolean
ol_app_download_lyric (OlMusicInfo *music_info)
{
  ol_log_func ();
  OlConfig *config = ol_config_get_instance ();
  char *name = ol_config_get_string (config, "Download", "download-engine");
  ol_debugf ("Download engine: %s\n", name);
  OlLrcFetchEngine *engine = ol_lrc_fetch_get_engine (name);
  ol_lrc_fetch_begin_search (engine, 
                             music_info, 
                             _search_callback,
                             NULL);
  if (module != NULL)
    ol_display_module_search_message (module, _("Searching lyrics"));
  return TRUE;
}

struct OlLrc *
ol_app_get_current_lyric ()
{
  ol_log_func ();
  return lrc_file;
}

gboolean
ol_app_assign_lrcfile (const OlMusicInfo *info,
                       const char *filepath,
                       gboolean update)
{
  ol_log_func ();
  ol_assert_ret (info != NULL, FALSE);
  ol_assert_ret (filepath == NULL || ol_path_is_file (filepath), FALSE);
  if (update)
  {
    ol_lrclib_assign_lyric (info, filepath);
  }
  if (ol_music_info_equal (&music_info, info))
  {
    if (lrc_file != NULL)
    {
      ol_lrc_free (lrc_file);
      lrc_file = NULL;
    }
    if (filepath != NULL)
      lrc_file = ol_lrc_new (filepath);
    ol_display_module_set_lrc (module, lrc_file);
  }
  return TRUE;
}

static gboolean
_check_lyric_file ()
{
  ol_log_func ();
  gboolean ret = TRUE;
  char *filename = NULL;
  int code = ol_lrclib_find (&music_info, &filename);
  if (code == 0)
    filename = ol_lyric_find (&music_info);
 
  if (filename != NULL)
  {
    ret = ol_app_assign_lrcfile (&music_info, filename, code == 0);
    g_free (filename);
  }
  else
  {
    ol_debugf("filename;%s\n", filename);
    if (code == 0) ret = FALSE;
  }
  return ret;
}

static void
_on_music_changed ()
{
  ol_log_func ();
  if (module != NULL)
  {
    ol_display_module_set_music_info (module, &music_info);
    ol_display_module_set_duration (module, previous_duration);
  }
  ol_display_module_set_lrc (module, NULL);
  if (!_check_lyric_file () &&
      !ol_is_string_empty (ol_music_info_get_title (&music_info)))
    ol_app_download_lyric (&music_info);
  OlConfig *config = ol_config_get_instance ();
  if (ol_config_get_bool (config, "General", "notify-music"))
    ol_notify_music_change (&music_info, ol_player_get_icon_path (player));
}

static void
_normalize_music_info (OlMusicInfo *music_info)
{
  if (ol_is_string_empty (ol_music_info_get_title (music_info)) &&
      ! ol_is_string_empty (ol_music_info_get_uri (music_info)))
  {
    const char *uri = ol_music_info_get_uri (music_info);
    char *path = NULL;
    if (uri[0] == '/')
    {
      path = g_strdup (uri);
    }
    else
    {
      GError *err = NULL;
      path = g_filename_from_uri (uri, NULL, &err);
      if (path == NULL)
      {
        ol_debugf ("Convert uri failed: %s\n", err->message);
        g_error_free (err);
      }
    }
    if (path != NULL)
    {
      char *basename = g_path_get_basename (path);
      char *mainname = NULL;
      ol_path_splitext (basename, &mainname, NULL);
      if (mainname != NULL)
      {
        ol_music_info_set_title (music_info, mainname);
        g_free (mainname);
      }
      g_free (basename);
      g_free (path);
    }
  }
}

static void
_check_music_change ()
{
  ol_log_func ();
  /* checks whether the music has been changed */
  gboolean changed = FALSE;
  /* compares the previous title with current title */
  if (player && !ol_player_get_music_info (player, &music_info))
  {
    player = NULL;
  }
  else
  {
    _normalize_music_info (&music_info);
  }
  gint duration = 0;
  if (player && !ol_player_get_music_length (player, &duration))
  {
    player = NULL;
  }
  if (!ol_streq (music_info.title, previous_title))
    changed = TRUE;
  ol_strptrcpy (&previous_title, music_info.title);
  /* compares the previous artist with current  */
  if (!ol_streq (music_info.artist, previous_artist))
    changed = TRUE;
  ol_strptrcpy (&previous_artist, music_info.artist);
  if (!ol_streq (music_info.uri, previous_uri))
    changed = TRUE;
  ol_strptrcpy (&previous_uri, music_info.uri);
  /* compares the previous duration */
  /* FIXME: because the a of banshee, some lyrics may return different
     duration for the same song when plays to different position, so the
     comparison is commented out temporarily */
  /* if (previous_duration != duration) */
  /* { */
  /*   ol_debugf ("change6:%d-%d\n", previous_duration, duration); */
  /*   changed = TRUE; */
  /* } */
  if (previous_duration != duration)
  {
    previous_duration = duration;
    if (module != NULL)
      ol_display_module_set_duration (module, duration);
  }
  if (changed)
  {
    _on_music_changed ();
  }
}

static void
_update_player_status (enum OlPlayerStatus status)
{
  ol_log_func ();
  if (previous_status != status)
  {
    previous_status = status;
    if (module != NULL)
    {
      ol_display_module_set_status (module, status);
    }
    ol_trayicon_status_changed (status);
  }
}

static gint
_refresh_player_info (gpointer data)
{
  ol_log_func ();
  if (player != NULL)
  {
    if (ol_player_get_capacity (player) & OL_PLAYER_STATUS)
      _update_player_status (ol_player_get_status (player));
    _check_music_change ();
  }
  return TRUE;
}

static gboolean
_get_active_player (void)
{
  ol_log_func ();
  player = ol_player_get_active_player ();
  if (player == NULL)
  {
    gboolean ignore = FALSE;
    if (first_run)
    {
      OlConfig *config = ol_config_get_instance ();
      char *player_cmd = ol_config_get_string (config,
                                               "General",
                                               "startup-player");
      if (!ol_is_string_empty (player_cmd))
      {
        ignore = TRUE;
        ol_debugf ("Running %s\n", player_cmd);
        g_spawn_command_line_async (player_cmd, NULL);
        sleep (5);
      }
      g_free (player_cmd);
    }
    if (!ignore)
    {
      printf (_("No supported player is running, exit.\n"));
      gtk_main_quit ();
    }
  }
  ol_display_module_set_player (module, player);
  first_run = FALSE;
  return player != NULL;
}

static gint
_refresh_music_info (gpointer data)
{
  ol_log_func ();
  //printf ("_refresh_music_info:successful\n");
  /* ol_log_func (); */
  if (player == NULL && !_get_active_player ())
    return TRUE;
  gint time = 0;
  if (player && !ol_player_get_played_time (player, &time))
  {
    player = NULL;
  }
  if (previous_position < 0 || time < previous_position ||
      previous_title == NULL)
    _check_music_change ();
  previous_position = time;
  if (player == NULL)
  {
    previous_position = -1;
    return TRUE;
  }
  ol_display_module_set_played_time (module, time);
  return TRUE;
}

struct OlPlayer*
ol_app_get_player ()
{
  return player;
}

OlMusicInfo*
ol_app_get_current_music ()
{
  return &music_info;
}

void
ol_app_adjust_lyric_offset (int offset_ms)
{
  ol_log_func ();
  struct OlLrc *lrc = ol_app_get_current_lyric ();
  if (lrc == NULL)
    return;
  int old_offset = ol_lrc_get_offset (lrc);
  int new_offset = old_offset - offset_ms;
  ol_lrc_set_offset (lrc, new_offset);
}

static void
_parse_cmd_args (int *argc, char ***argv)
{
  ol_log_func ();
  GError *error = NULL;
  GOptionContext *context;

  context = g_option_context_new ("- Display your lyrics");
  g_option_context_add_main_entries (context, cmdargs, PACKAGE);
  g_option_context_add_group (context, gtk_get_option_group (TRUE));
  if (!g_option_context_parse (context, argc, argv, &error))
  {
    ol_errorf ("option parsing failed: %s\n", error->message);
  }
  if (_arg_version)
  {
    printf ("%s\n", PACKAGE_STRING);
    exit (0);
  }
}

gboolean
_arg_debug_cb (const gchar *option_name,
               const gchar *value,
               gpointer data,
               GError **error)
{
  if (value == NULL)
    value = "debug";
  if (strcmp (value, "none") == 0)
  {
    ol_log_set_level (OL_LOG_NONE);
  }
  else if (strcmp (value, "error") == 0)
  {
    ol_log_set_level (OL_ERROR);
  }
  else if (strcmp (value, "debug") == 0)
  {
    ol_log_set_level (OL_DEBUG);
  }
  else if (strcmp (value, "info") == 0)
  {
    ol_log_set_level (OL_INFO);
  }
  else
  {
    g_set_error_literal (error, g_quark_from_string (PACKAGE_NAME), 1,
                         N_ ("debug level should be one of ``none'', ``error'', ``debug'', or ``info''"));
    return FALSE;
  }
  return TRUE;
}

static void
_initialize (int argc, char **argv)
{
  ol_log_func ();
#if ENABLE_NLS
  /* Set the text message domain.  */
  bindtextdomain (PACKAGE, LOCALEDIR);
  bind_textdomain_codeset(PACKAGE, "UTF-8");
  /* textdomain (PACKAGE); */
#endif
  /* Handler for SIGCHLD to wait lrc downloading process */
  /* signal (SIGCHLD, child_handler); */

  g_thread_init(NULL);
  gtk_init (&argc, &argv);
  _parse_cmd_args (&argc, &argv);
  if (ol_is_running ())
  {
    printf ("%s\n", _("Another OSD Lyrics is running, exit."));
    exit (0);
  }
  ol_stock_init ();
  ol_player_init ();
  /* Initialize display modules */
  ol_display_module_init ();
  OlConfig *config = ol_config_get_instance ();
  display_mode = ol_config_get_string (config, "General", "display-mode");
  module = ol_display_module_new (display_mode);
  g_signal_connect (config, "changed",
                    G_CALLBACK (_on_config_changed),
                    NULL);

  ol_trayicon_inital ();
  ol_notify_init ();
  ol_keybinding_init ();
  ol_lrc_fetch_module_init ();
  char *lrcdb_file = g_strdup_printf ("%s/%s/%s",
                                      g_get_user_config_dir (),
                                      PACKAGE_NAME,
                                      LRCDB_FILENAME);
  if (!ol_lrclib_init (lrcdb_file))
  {
    ol_error ("Initialize lrclib failed");
  }
  g_free (lrcdb_file);
  ol_lrc_fetch_add_async_download_callback (_download_callback);
  refresh_source = g_timeout_add (REFRESH_INTERVAL, _refresh_music_info, NULL);
  info_timer = g_timeout_add (INFO_INTERVAL, _refresh_player_info, NULL);
}

int
main (int argc, char **argv)
{
  _initialize (argc, argv);
  gtk_main ();
  ol_player_unload ();
  ol_notify_unload ();
  ol_display_module_free (module);
  if (display_mode != NULL) g_free (display_mode);
  display_mode = NULL;
  module = NULL;
  ol_display_module_unload ();
  ol_trayicon_free ();
  ol_lrclib_unload ();
  ol_config_unload ();
  return 0;
}
