/* -*- mode: C; c-basic-offset: 2; indent-tabs-mode: nil; -*- */
/*
 * Copyright (C) 2009-2011  Tiger Soldier <tigersoldier@gmail.com>
 *
 * This file is part of OSD Lyrics.
 * 
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
/* http://git.gnome.org/cgit/rhythmbox/tree/shell/rb-shell-player.xml */
#include "ol_player_rhythmbox.h"
#include "ol_utils_dbus.h"
#include "ol_utils.h"
#include "ol_debug.h"
#include "ol_elapse_emulator.h"

static const char SERVICE[] = "org.gnome.Rhythmbox";
static const char PATH_PLAYER[] = "/org/gnome/Rhythmbox/Player";
static const char INTERFACE_PLAYER[] = "org.gnome.Rhythmbox.Player";
static const char PATH_SHELL[] = "/org/gnome/Rhythmbox/Shell";
static const char INTERFACE_SHELL[] = "org.gnome.Rhythmbox.Shell";
static const char GET_URI[] = "getPlayingUri";
static const char GET_PROPERTIES[] = "getSongProperties";
static const char PROP_TITLE[] = "title";
static const char PROP_ARTIST[] = "artist";
static const char PROP_ALBUM[] = "album";
static const char PROP_TRACK[] = "track-number";
static const char PROP_URI[] = "uri";
static const char PLAY_PAUSE[] = "playPause";
static const char PREVIOUSE[] = "previous";
static const char NEXT[] = "next";
static const char GET_PLAYING[] = "getPlaying";
static const char SEEK[] = "setElapsed";
static const char *icon_paths[] = {
  "/usr/share/icons/hicolor/scalable/apps/rhythmbox.svg",
  "/usr/share/icons/hicolor/48x48/apps/rhythmbox.png",
  "/usr/local/share/icons/hicolor/scalable/apps/rhythmbox.svg",
  "/usr/local/share/icons/hicolor/48x48/apps/rhythmbox.png",
};
static GHashTable *STOP_PROPERTIES = (GHashTable*)-1;

static DBusGProxy *proxy_player = NULL;
static DBusGProxy *proxy_shell = NULL;
static GError *error = NULL;
static int ol_played_time;

static gboolean ol_player_rhythmbox_get_music_info (OlMusicInfo *info);
static gboolean ol_player_rhythmbox_get_played_time (int *played_time);
static gboolean ol_player_rhythmbox_get_music_length (int *len);
static GHashTable* ol_player_rhythmbox_get_song_properties ();
static gboolean ol_player_rhythmbox_init_dbus ();
static gboolean ol_player_rhythmbox_get_activated ();
static gboolean ol_player_rhythmbox_proxy_destroy_handler ();
static void ol_player_rhythmbox_elapsed_changed (DBusGProxy *player_proxy, int elapsed, gpointer data);
static int ol_player_rhythmbox_get_capacity ();
static enum OlPlayerStatus ol_player_rhythmbox_get_status ();
static gboolean ol_player_rhythmbox_play ();
static gboolean ol_player_rhythmbox_pause ();
static gboolean ol_player_rhythmbox_stop ();
static gboolean ol_player_rhythmbox_prev ();
static gboolean ol_player_rhythmbox_next ();
static gboolean ol_player_rhythmbox_seek (int pos_ms);
static const char *ol_player_rhythmbox_get_icon_path ();

static OlElapseEmulator elapse;


/** 
 * Return real position in milliseconds
 * Because Rhythmbox return position only in seconds, which means the position
 * is rounded to 1000, so we need to simulate a timer to get proper time
 * in milliseconds
 * @param time rounded time in milliseconds
 * 
 * @return 
 */
static int
ol_player_rhythmbox_get_real_ms (int time)
{
  if (ol_player_rhythmbox_get_status () != OL_PLAYER_PLAYING)
  {
    int real_time = ol_elapse_emulator_get_last_ms (&elapse, time);
    /* reset timer to current time to avoid error on resume playing */
    ol_elapse_emulator_init (&elapse, real_time, elapse.accuracy);
    return real_time;
  }
  else
    return ol_elapse_emulator_get_real_ms (&elapse, time);
}

static gboolean
ol_player_rhythmbox_proxy_destroy_handler (DBusGProxy *proxy, gboolean shell)
{
  ol_log_func ();
  if (proxy == proxy_shell)
  {
    g_object_unref (proxy_shell);
    proxy_shell = NULL;
  }
  else
  {
    g_object_unref (proxy_player);
    proxy_player = NULL;
  }
  return FALSE;
}

static void
ol_player_rhythmbox_elapsed_changed (DBusGProxy *player_proxy, int elapsed, gpointer data)
{
  ol_played_time = elapsed * 1000;
}

static GHashTable*
ol_player_rhythmbox_get_song_properties ()
{
  /* ol_log_func (); */
  if (proxy_player == NULL || proxy_shell == NULL)
    if (!ol_player_rhythmbox_init_dbus ())
      return NULL;
  GHashTable *data_list = NULL;
  char *uri = NULL;
  if (ol_dbus_get_string (proxy_player, GET_URI, &uri))
  {
    if (dbus_g_proxy_call (proxy_shell, GET_PROPERTIES, NULL,
                           G_TYPE_STRING, uri,
                           G_TYPE_INVALID,
                           dbus_g_type_get_map ("GHashTable", G_TYPE_STRING, G_TYPE_VALUE),
                           &data_list,
                           G_TYPE_INVALID))
    {
    }
    else
    {
      /* ol_logf (OL_DEBUG, "  failed, uri:%s\n", uri); */
      data_list = STOP_PROPERTIES;
    }
    g_free (uri);
  }
  return data_list;
}

static gboolean
ol_player_rhythmbox_get_music_info (OlMusicInfo *info)
{
  /* ol_log_func (); */
  ol_assert_ret (info != NULL, FALSE);
  if (proxy_player == NULL || proxy_shell == NULL)
    if (!ol_player_rhythmbox_init_dbus ())
      return FALSE;
  GHashTable *data_list;
  gboolean ret = TRUE;
  data_list = ol_player_rhythmbox_get_song_properties ();
  if (data_list)
  {
    ol_music_info_clear (info);
    if (data_list != STOP_PROPERTIES)
    {
      info->artist = g_strdup (ol_get_string_from_hash_table (data_list, PROP_ARTIST));
      info->title = g_strdup (ol_get_string_from_hash_table (data_list, PROP_TITLE));
      info->album = g_strdup (ol_get_string_from_hash_table (data_list, PROP_ALBUM));
      info->track_number = ol_get_int_from_hash_table (data_list, PROP_TRACK);
      ol_dbus_get_string (proxy_player, GET_URI, &info->uri);
      g_hash_table_destroy (data_list);
      /* ol_debugf("  artist:%s\n" */
      /*           "  title:%s\n" */
      /*           "  album:%s\n" */
      /*           "  track:%d\n" */
      /*           "  uri:%s\n", */
      /*           info->artist, */
      /*           info->title, */
      /*           info->album, */
      /*           info->track_number, */
      /*           info->uri); */
    }
  }
  else
  {
    ret = FALSE;
  }
  return ret;
}

static gboolean
ol_player_rhythmbox_get_played_time (int *played_time)
{
  /* ol_log_func (); */
  ol_assert_ret (played_time != NULL, FALSE);
  if (!played_time)
    return FALSE;
  if ((!proxy_player || !proxy_shell) && !ol_player_rhythmbox_init_dbus ())
    return FALSE;
  *played_time = ol_player_rhythmbox_get_real_ms (ol_played_time);
  return TRUE;
}

static gboolean
ol_player_rhythmbox_get_music_length (int *len)
{
  /* ol_log_func (); */
  ol_assert_ret (len != NULL, FALSE);
  GHashTable *data_list = ol_player_rhythmbox_get_song_properties ();
  if (data_list == NULL)
    return FALSE;
  if (data_list == STOP_PROPERTIES)
  {
    *len = 0;
  }
  else
  {
    *len = ol_get_uint_from_hash_table (data_list, "duration") * 1000;
    g_hash_table_destroy (data_list);
  }
  return TRUE;
}

static gboolean
ol_player_rhythmbox_get_activated ()
{
  if ((!proxy_player || !proxy_shell) && !ol_player_rhythmbox_init_dbus ())
    return FALSE;
  return TRUE;
}

static gboolean
ol_player_rhythmbox_init_dbus ()
{
  ol_log_func ();
  DBusGConnection *connection = ol_dbus_get_connection ();
  if (connection == NULL)
  {
    return FALSE;
  }
  /* if (proxy_player != NULL) */
  /* { */
  /*   g_object_unref (proxy_player); */
  /*   proxy_player = NULL; */
  /* } */
  if (proxy_player == NULL)
  {
    proxy_player = dbus_g_proxy_new_for_name_owner (connection, SERVICE, PATH_PLAYER, INTERFACE_PLAYER, &error);
    if (proxy_player == NULL)
    {
      ol_infof ("get proxy failed: %s\n", error->message);
      g_error_free (error);
      error = NULL;
      return FALSE;
    }
    g_signal_connect (proxy_player, "destroy", G_CALLBACK (ol_player_rhythmbox_proxy_destroy_handler), NULL);
    dbus_g_proxy_add_signal(proxy_player, "elapsedChanged",
                            G_TYPE_UINT,
                            G_TYPE_INVALID);
    dbus_g_proxy_connect_signal(proxy_player, "elapsedChanged",
                                G_CALLBACK(ol_player_rhythmbox_elapsed_changed), NULL, NULL);
  }
  if (proxy_shell == NULL)
  {
    proxy_shell = dbus_g_proxy_new_for_name_owner (connection, SERVICE, PATH_SHELL, INTERFACE_SHELL, &error);
    if (proxy_shell == NULL)
    {
      ol_errorf ("get proxy failed: %s\n", error->message);
      g_error_free (error);
      error = NULL;
      return FALSE;
    }
    g_signal_connect (proxy_shell, "destroy", G_CALLBACK (ol_player_rhythmbox_proxy_destroy_handler), NULL);
  }
  return TRUE;
}

static int
ol_player_rhythmbox_get_capacity ()
{
  return OL_PLAYER_STATUS | OL_PLAYER_PLAY | OL_PLAYER_PAUSE | OL_PLAYER_STOP |
    OL_PLAYER_PREV | OL_PLAYER_NEXT | OL_PLAYER_SEEK;
}

static enum OlPlayerStatus
ol_player_rhythmbox_get_status ()
{
  /* ol_log_func (); */
  if (proxy_player == NULL || proxy_shell == NULL)
    if (!ol_player_rhythmbox_init_dbus ())
      return OL_PLAYER_ERROR;
  gboolean playing;
  if (ol_dbus_get_bool (proxy_player, GET_PLAYING, &playing))
  {
    if (playing)
      return OL_PLAYER_PLAYING;
    else
      return OL_PLAYER_PAUSED;
  }
  return OL_PLAYER_ERROR;
}

static gboolean
ol_player_rhythmbox_play ()
{
  ol_log_func ();
  enum OlPlayerStatus status = ol_player_rhythmbox_get_status ();
  if (status == OL_PLAYER_ERROR)
    return FALSE;
  if (status != OL_PLAYER_PLAYING)
    return dbus_g_proxy_call (proxy_player,
                              PLAY_PAUSE,
                              NULL,
                              G_TYPE_BOOLEAN,
                              TRUE, /* unused */
                              G_TYPE_INVALID,
                              G_TYPE_INVALID);
  else
    return TRUE;
}

static gboolean
ol_player_rhythmbox_pause ()
{
  ol_log_func ();
  enum OlPlayerStatus status = ol_player_rhythmbox_get_status ();
  if (status == OL_PLAYER_ERROR)
    return FALSE;
  if (status == OL_PLAYER_PLAYING)
    return dbus_g_proxy_call (proxy_player,
                              PLAY_PAUSE,
                              NULL,
                              G_TYPE_BOOLEAN,
                              TRUE, /* unused */
                              G_TYPE_INVALID,
                              G_TYPE_INVALID);
  else
    return TRUE;
}

static gboolean
ol_player_rhythmbox_stop ()
{
  ol_log_func ();
  if (!ol_player_rhythmbox_pause ())
    return FALSE;
  return ol_player_rhythmbox_seek (0);
}

static gboolean
ol_player_rhythmbox_prev ()
{
  ol_log_func ();
  if (proxy_player == NULL || proxy_shell == NULL)
    if (!ol_player_rhythmbox_init_dbus ())
      return FALSE;
  return ol_dbus_invoke (proxy_player, PREVIOUSE);
}

static gboolean
ol_player_rhythmbox_next ()
{
  ol_log_func ();
  if (proxy_player == NULL || proxy_shell == NULL)
    if (!ol_player_rhythmbox_init_dbus ())
      return FALSE;
  return ol_dbus_invoke (proxy_player, NEXT);
}

static gboolean
ol_player_rhythmbox_seek (int pos_ms)
{
  ol_log_func ();
  if (proxy_player == NULL || proxy_shell == NULL)
    if (!ol_player_rhythmbox_init_dbus ())
      return FALSE;
  return dbus_g_proxy_call (proxy_player,
                            SEEK,
                            NULL,
                            G_TYPE_UINT,
                            (guint) pos_ms / 1000,
                            G_TYPE_INVALID,
                            G_TYPE_INVALID);
}

static const char *
ol_player_rhythmbox_get_icon_path ()
{
  int i;
  for (i = 0; i < ol_get_array_len (icon_paths); i++)
  {
    if (ol_path_is_file (icon_paths[i]))
      return icon_paths[i];
  }
  return NULL;
}

struct OlPlayer*
ol_player_rhythmbox_get ()
{
  ol_log_func ();
  ol_elapse_emulator_init (&elapse, 0, 1000);
  struct OlPlayer *controller = ol_player_new ("Rhythmbox");
  ol_player_set_cmd (controller, "rhythmbox");
  controller->get_music_info = ol_player_rhythmbox_get_music_info;
  controller->get_activated = ol_player_rhythmbox_get_activated;
  controller->get_played_time = ol_player_rhythmbox_get_played_time;
  controller->get_music_length = ol_player_rhythmbox_get_music_length;
  controller->get_capacity = ol_player_rhythmbox_get_capacity;
  controller->get_status = ol_player_rhythmbox_get_status;
  controller->play = ol_player_rhythmbox_play;
  controller->pause = ol_player_rhythmbox_pause;
  controller->prev = ol_player_rhythmbox_prev;
  controller->next = ol_player_rhythmbox_next;
  controller->seek = ol_player_rhythmbox_seek;
  controller->stop = ol_player_rhythmbox_stop;
  controller->get_icon_path = ol_player_rhythmbox_get_icon_path;
  return controller;
}
