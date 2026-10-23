/* Teléfonos y cámaras vía gvfs, y el escaneo completo (UdScan).
 *
 * Un teléfono en modo "transferencia de archivos" habla MTP: no hay dispositivo
 * de bloque, así que UDisks2 no lo ve y no se puede montar como un pendrive.
 * gvfs (gvfs-mtp, gvfs-gphoto2, gvfs-afc) lo expone como un GVolume, y al
 * montarlo como un GMount con raíz mtp://, gphoto2:// o afc://.
 *
 * Sin gvfs no aparece nada acá, y el aparato queda en "Otros dispositivos"
 * con tipo UD_USB_PHONE (ver ud_get_usb_devices). */

#include "ud-dbus.h"
#include <string.h>

/* devuelve el tipo si la URI es de teléfono/cámara, -1 si no */
static gint
phone_kind_from_uri (const gchar *uri)
{
    gint   kind   = -1;
    gchar *scheme = uri ? g_uri_parse_scheme (uri) : NULL;

    if (g_strcmp0 (scheme, "mtp") == 0)          kind = UD_PHONE_MTP;
    else if (g_strcmp0 (scheme, "gphoto2") == 0) kind = UD_PHONE_PTP;
    else if (g_strcmp0 (scheme, "afc") == 0)     kind = UD_PHONE_AFC;

    g_free (scheme);
    return kind;
}

static gchar *
file_uri_or_null (GFile *f)
{
    return f ? g_file_get_uri (f) : NULL;
}

static UdPhone *
phone_from_volume (GVolume *vol)
{
    GFile  *act       = g_volume_get_activation_root (vol);
    GMount *mount     = g_volume_get_mount (vol);
    GFile  *root      = mount ? g_mount_get_root (mount) : NULL;
    gchar  *act_uri   = file_uri_or_null (act);
    gchar  *root_uri  = file_uri_or_null (root);
    gint    kind      = phone_kind_from_uri (act_uri ? act_uri : root_uri);
    UdPhone *ph       = NULL;

    if (kind < 0 && root_uri)
        kind = phone_kind_from_uri (root_uri);

    if (kind >= 0) {
        ph          = g_new0 (UdPhone, 1);
        ph->kind    = (UdPhoneKind) kind;
        ph->name    = g_volume_get_name (vol);
        ph->icon    = g_volume_get_symbolic_icon (vol);
        ph->uri     = root_uri ? g_strdup (root_uri) : g_strdup (act_uri);
        ph->devfile = g_volume_get_identifier (vol, G_VOLUME_IDENTIFIER_KIND_UNIX_DEVICE);
        ph->key     = g_strdup (ph->devfile ? ph->devfile : ph->uri);
        ph->volume  = g_object_ref (vol);
        ph->mount   = mount ? g_object_ref (mount) : NULL;
    }

    g_free (act_uri);
    g_free (root_uri);
    if (act)   g_object_unref (act);
    if (root)  g_object_unref (root);
    if (mount) g_object_unref (mount);
    return ph;
}

/* montajes sin volumen (p. ej. afc de un iPhone montado a mano) */
static UdPhone *
phone_from_orphan_mount (GMount *mount)
{
    GFile   *root     = g_mount_get_root (mount);
    gchar   *root_uri = file_uri_or_null (root);
    gint     kind     = phone_kind_from_uri (root_uri);
    UdPhone *ph       = NULL;

    if (kind >= 0) {
        ph        = g_new0 (UdPhone, 1);
        ph->kind  = (UdPhoneKind) kind;
        ph->name  = g_mount_get_name (mount);
        ph->icon  = g_mount_get_symbolic_icon (mount);
        ph->uri   = root_uri;
        root_uri  = NULL;
        ph->key   = g_strdup (ph->uri);
        ph->mount = g_object_ref (mount);
    }

    g_free (root_uri);
    if (root) g_object_unref (root);
    return ph;
}

static gint
compare_phones (gconstpointer a, gconstpointer b)
{
    const UdPhone *pa = a, *pb = b;
    gint c = g_utf8_collate (pa->name ? pa->name : "", pb->name ? pb->name : "");
    return (c != 0) ? c : g_strcmp0 (pa->key, pb->key);
}

GSList *
ud_get_phones (void)
{
    GVolumeMonitor *vm     = g_volume_monitor_get ();
    GSList         *result = NULL;

    GList *vols = g_volume_monitor_get_volumes (vm);
    for (GList *l = vols; l; l = l->next) {
        UdPhone *ph = phone_from_volume (l->data);
        if (ph)
            result = g_slist_prepend (result, ph);
    }
    g_list_free_full (vols, g_object_unref);

    GList *mounts = g_volume_monitor_get_mounts (vm);
    for (GList *l = mounts; l; l = l->next) {
        GMount  *m   = l->data;
        GVolume *vol = g_mount_get_volume (m);
        if (vol) {              /* ya cubierto por su volumen (o no es teléfono) */
            g_object_unref (vol);
            continue;
        }
        if (g_mount_is_shadowed (m))
            continue;
        UdPhone *ph = phone_from_orphan_mount (m);
        if (ph)
            result = g_slist_prepend (result, ph);
    }
    g_list_free_full (mounts, g_object_unref);

    g_object_unref (vm);
    return g_slist_sort (result, compare_phones);
}

void
ud_phone_list_free (GSList *list)
{
    for (GSList *l = list; l; l = l->next) {
        UdPhone *ph = l->data;
        g_free (ph->key);
        g_free (ph->name);
        g_free (ph->uri);
        g_free (ph->devfile);
        if (ph->icon)   g_object_unref (ph->icon);
        if (ph->volume) g_object_unref (ph->volume);
        if (ph->mount)  g_object_unref (ph->mount);
        g_free (ph);
    }
    g_slist_free (list);
}

/* ---------- escaneo completo ---------- */

UdScan *
ud_scan (GDBusConnection *conn)
{
    UdScan *scan  = g_new0 (UdScan, 1);
    scan->storage = ud_get_devices (conn);
    scan->phones  = ud_get_phones ();
    scan->usb     = ud_get_usb_devices ();

    /* el aparato que gvfs ya muestra como teléfono no se repite en "Otros" */
    GHashTable *taken = g_hash_table_new (g_str_hash, g_str_equal);
    for (GSList *l = scan->phones; l; l = l->next) {
        UdPhone *ph = l->data;
        if (ph->devfile)
            g_hash_table_add (taken, ph->devfile);
    }

    GSList *l = scan->usb;
    while (l) {
        GSList      *next = l->next;
        UdUsbDevice *u    = l->data;
        if (u->devfile && g_hash_table_contains (taken, u->devfile)) {
            scan->usb = g_slist_remove_link (scan->usb, l);
            ud_usb_device_list_free (l);   /* libera ese único nodo y su dato */
        }
        l = next;
    }
    g_hash_table_destroy (taken);

    return scan;
}

void
ud_scan_free (UdScan *scan)
{
    if (!scan)
        return;
    ud_device_list_free     (scan->storage);
    ud_phone_list_free      (scan->phones);
    ud_usb_device_list_free (scan->usb);
    g_free (scan);
}

gboolean
ud_scan_is_empty (const UdScan *scan)
{
    return !scan || (!scan->storage && !scan->phones && !scan->usb);
}
