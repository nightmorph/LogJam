/* logjam - a GTK client for LiveJournal.
 * Copyright (C) 2009 Andy Shevchenko <andy@smile.org.ua>
 *
 * vim: tabstop=4 shiftwidth=4 noexpandtab :
 *
 * See http://wiki.xmms2.xmms.se/wiki/MPRIS for MRPIS specification
 */

#include <string.h>		/* memset */

#include "lj_dbus.h"

#define MPRIS_IF	"org.freedesktop.MediaPlayer"

/* Internal prototypes */
static gboolean lj_dbus_open(JamDBus *jd);
static gboolean lj_dbus_append_player(JamDBus *jd, gchar *dest);
static void lj_dbus_players_clear(JamDBus *jd);
static gboolean lj_dbus_players_find(JamDBus *jd, GError **error);

/* Implementation */
static gboolean
lj_dbus_open(JamDBus *jd) {
	GError *error = NULL;

	jd->bus = dbus_g_bus_get(DBUS_BUS_SESSION, &error);
	if (jd->bus == NULL) {
		g_printerr("Failed to open connection to bus: %s\n", error->message);
		g_error_free(error);
		return FALSE;
    }
	return TRUE;
}

void lj_dbus_close(JamDBus *jd) {
	if (jd == NULL)
		return;
	lj_dbus_players_clear(jd);
	dbus_g_connection_unref(jd->bus);
}

JamDBus *
lj_dbus_new(void) {
	JamDBus *jd = (JamDBus *) g_malloc0(sizeof(JamDBus));

	if (lj_dbus_open(jd) == FALSE) {
		g_free(jd);
		return NULL;
	}
	return jd;
}

static gboolean
lj_dbus_append_player(JamDBus *jd, gchar *dest) {
	MediaPlayer *player;
	DBusGProxy *proxy;
	GError *error = NULL;
	gchar *name;

	proxy = dbus_g_proxy_new_for_name(jd->bus, dest, "/", MPRIS_IF);

	if (!dbus_g_proxy_call(proxy, "Identity", &error, G_TYPE_INVALID,
						   G_TYPE_STRING, &name, G_TYPE_INVALID)) {
		g_printerr("Error: %s\n", error->message);
		g_error_free(error);
		return FALSE;
	}

	player = (MediaPlayer *) g_malloc0(sizeof(MediaPlayer));
	player->dest = g_strdup(dest);
	player->name = g_strdup(name);
	player->proxy = dbus_g_proxy_new_for_name(jd->bus, dest, "/Player", MPRIS_IF);

	if (g_str_has_suffix(dest, "audacious"))
		player->hint |= MPRIS_HINT_BAD_STATUS;

	jd->player = g_list_append(jd->player, (gpointer) player);

	g_free(name);
	g_object_unref(proxy);

	return TRUE;
}

static void
lj_dbus_players_clear(JamDBus *jd) {
	GList *list;

	if (jd->player == NULL)
		return;

	for (list = g_list_first(jd->player); list; list = g_list_next(list)) {
		MediaPlayer *player = (MediaPlayer *) list->data;
		g_object_unref(player->proxy);
		if (player->name)
			g_free(player->name);
		if (player->dest)
			g_free(player->dest);
	}
	g_list_free(jd->player);
	jd->player = NULL;
}

static gboolean
lj_dbus_players_find(JamDBus *jd, GError **error) {
	DBusGProxy *proxy;
	gchar **names, **p;

	proxy = dbus_g_proxy_new_for_name(jd->bus,
                                      DBUS_SERVICE_DBUS,
                                      DBUS_PATH_DBUS,
                                      DBUS_INTERFACE_DBUS);

	if (!dbus_g_proxy_call(proxy, "ListNames", error, G_TYPE_INVALID,
						   G_TYPE_STRV, &names, G_TYPE_INVALID)) {
		return FALSE;
	}

	for (p = names; *p; p++) {
		if (g_str_has_prefix(*p, "org.mpris.")) {
			lj_dbus_append_player(jd, *p);
		}
	}

	g_strfreev(names);
	g_object_unref(proxy);

	return TRUE;
}

gboolean
lj_dbus_mpris_update_list(JamDBus *jd, GError **error) {
	if (jd == NULL)
		return FALSE;
	lj_dbus_players_clear(jd);
	return lj_dbus_players_find(jd, error);
}

#define DBUS_TYPE_MPRIS_STATUS \
	(dbus_g_type_get_struct("GValueArray", G_TYPE_INT, G_TYPE_INT, G_TYPE_INT, G_TYPE_INT, G_TYPE_INVALID))
#define DBUS_TYPE_G_STRING_VALUE_HASHTABLE \
	(dbus_g_type_get_map("GHashTable", G_TYPE_STRING, G_TYPE_VALUE))

/* TODO: Connect to status change signal */

gboolean
lj_dbus_mpris_update_info(JamDBus *jd, GList *list, GError **error) {
	GValueArray *array = NULL;
	GHashTable *info = NULL;
	GValue *value;
	MediaPlayer *player;

	if (jd == NULL)
		return FALSE;

	if (list == NULL)
		return FALSE;

	if ((player = (MediaPlayer *) list->data) == NULL)
		return FALSE;

	memset((void *) &player->info, 0, sizeof(MetaInfo));

	if (player->hint & MPRIS_HINT_BAD_STATUS) {
		if (!dbus_g_proxy_call(player->proxy, "GetStatus", error, G_TYPE_INVALID,
							   G_TYPE_INT, &player->info.status, G_TYPE_INVALID)) {
			return FALSE;
		}
	} else {
		if (!dbus_g_proxy_call(player->proxy, "GetStatus", error, G_TYPE_INVALID,
							   DBUS_TYPE_MPRIS_STATUS, &array, G_TYPE_INVALID)) {
			return FALSE;
		}

		value = g_value_array_get_nth(array, 0);
		player->info.status = g_value_get_int(value);
		g_value_array_free(array);
	}

	if (player->info.status == MPRIS_STATUS_PLAYING) {
		if (!dbus_g_proxy_call(player->proxy, "GetMetadata", error, G_TYPE_INVALID,
							   DBUS_TYPE_G_STRING_VALUE_HASHTABLE, &info, G_TYPE_INVALID)) {
			return FALSE;
		}
		value = (GValue *) g_hash_table_lookup(info, "artist");
		if (value != NULL && G_VALUE_HOLDS_STRING(value)) {
			g_strlcpy(player->info.artist, g_value_get_string(value), MPRIS_INFO_LEN);
		}

		value = (GValue *) g_hash_table_lookup(info, "album");
		if (value != NULL && G_VALUE_HOLDS_STRING(value)) {
			g_strlcpy(player->info.album, g_value_get_string(value), MPRIS_INFO_LEN);
		}

		value = (GValue *) g_hash_table_lookup(info, "title");
		if (value != NULL && G_VALUE_HOLDS_STRING(value)) {
			g_strlcpy(player->info.title, g_value_get_string(value), MPRIS_INFO_LEN);
		}
	}

	return TRUE;
}

GQuark
lj_dbus_error_quark(void) {
	static GQuark quark = 0;
	if (quark == 0)
		quark = g_quark_from_static_string("dbus-error-quark");
	return quark;
}

/* TODO: User defined format */

gchar *
lj_dbus_mpris_current_music(JamDBus *jd, GError **error) {
	gchar *music;
	GList *list;

	if (!lj_dbus_mpris_update_list(jd, error))
		return NULL;

	list = jd ? jd->player : NULL;

	if (lj_dbus_mpris_update_info(jd, list, error)) {
		MediaPlayer *player = (MediaPlayer *) list->data;
		if (player->info.status == MPRIS_STATUS_PLAYING) {
			music = g_strdup_printf("%s - %s - %s",
						player->info.artist[0] ? player->info.artist : _("Unknown Artist"),
						player->info.album[0] ? player->info.album: _("Unknown Album"),
						player->info.title[0] ? player->info.title: _("Unknown Track"));
			return music;
		} else {
			g_set_error(error, lj_dbus_error_quark(), MPRIS_ERROR_NOT_PLAYING,
				_("Player is stopped."));
		}
	} else if (error == NULL || *error == NULL) {
		g_set_error(error, lj_dbus_error_quark(), MPRIS_ERROR_NO_PLAYER,
			_("No players found."));
	}
	return NULL;
}

/* lj_dbus.c */

