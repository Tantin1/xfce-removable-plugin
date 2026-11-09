#include "rp-notify.h"
#include <libnotify/notify.h>

/* Tablas del proceso. En Wayland todas las instancias del plugin comparten el
 * proceso del panel: dos instancias que avisan lo mismo del mismo aparato
 * terminan en una sola burbuja, que es lo deseable. */
static GHashTable *bubbles  = NULL;   /* clave → NotifyNotification* (ref) */
static GHashTable *expected = NULL;   /* clave → gint64 (hasta cuándo vale la marca) */

/* cuánto dura la marca de "desconexión esperada": UDisks2 quita el disco en
   menos de un segundo; el margen cubre el debounce y un sistema cargado */
#define EXPECT_REMOVAL_USEC (10 * G_USEC_PER_SEC)

typedef struct {
    gchar              *key;
    NotifyNotification *n;   /* sin ref: solo para comparar */
} ClosedData;

/* fuera de la emisión de "closed": soltar la última referencia adentro del
   manejador liberaría el objeto mientras libnotify todavía lo usa */
static gboolean
forget_bubble (gpointer data)
{
    ClosedData *cd = data;
    /* solo si sigue siendo la misma burbuja (pudo haberse reemplazado) */
    if (bubbles && g_hash_table_lookup (bubbles, cd->key) == cd->n)
        g_hash_table_remove (bubbles, cd->key);
    g_free (cd->key);
    g_free (cd);
    return G_SOURCE_REMOVE;
}

static void
free_key (gpointer key, GClosure *closure)
{
    (void) closure;
    g_free (key);
}

static void
on_bubble_closed (NotifyNotification *n, gpointer key)
{
    ClosedData *cd = g_new0 (ClosedData, 1);
    cd->key = g_strdup (key);
    cd->n   = n;
    g_idle_add (forget_bubble, cd);
}

void
rp_notify (const gchar *key, const gchar *summary, const gchar *body, const gchar *icon)
{
    NotifyNotification *n = NULL;

    if (key) {
        if (!bubbles)
            bubbles = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_object_unref);
        n = g_hash_table_lookup (bubbles, key);
    }

    if (n) {
        /* misma burbuja, mismo id: el servicio de notificaciones la reemplaza.
           Si ya se había cerrado, la muestra de nuevo */
        notify_notification_update (n, summary, body, icon);
        g_object_ref (n);
    } else {
        n = notify_notification_new (summary, body, icon);
        if (key) {
            g_hash_table_replace (bubbles, g_strdup (key), g_object_ref (n));
            g_signal_connect_data (n, "closed", G_CALLBACK (on_bubble_closed),
                                   g_strdup (key), free_key, 0);
        }
    }

    notify_notification_set_timeout (n, NOTIFY_EXPIRES_DEFAULT);
    notify_notification_show (n, NULL);
    g_object_unref (n);
}

void
rp_notify_close_all (void)
{
    if (!bubbles)
        return;
    GHashTableIter it;
    gpointer       n;
    g_hash_table_iter_init (&it, bubbles);
    while (g_hash_table_iter_next (&it, NULL, &n))
        notify_notification_close (n, NULL);   /* si ya estaba cerrada, el servicio lo ignora */
    /* olvidarlas: el próximo aviso de cada aparato es una burbuja nueva */
    g_hash_table_remove_all (bubbles);
}

void
rp_notify_expect_removal (const gchar *key)
{
    if (!key)
        return;
    if (!expected)
        expected = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
    gint64 *until = g_new (gint64, 1);
    *until = g_get_monotonic_time () + EXPECT_REMOVAL_USEC;
    g_hash_table_replace (expected, g_strdup (key), until);
}

gboolean
rp_notify_removal_expected (const gchar *key)
{
    if (!key || !expected)
        return FALSE;
    gint64 *until = g_hash_table_lookup (expected, key);
    if (!until)
        return FALSE;
    gboolean valid = g_get_monotonic_time () <= *until;
    g_hash_table_remove (expected, key);   /* se consume (o vencía) */
    return valid;
}
