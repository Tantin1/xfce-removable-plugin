#include "ud-dbus.h"
#include <string.h>
#include <gudev/gudev.h>

/* ---------- conexión ---------- */

GDBusConnection *
ud_dbus_connect (void)
{
    GError          *err  = NULL;
    GDBusConnection *conn = g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, &err);
    if (!conn) {
        g_warning ("ud-dbus: no se pudo conectar: %s",
                   err ? err->message : "desconocido");
        g_clear_error (&err);
    }
    return conn;
}

/* ---------- helpers ---------- */

static gchar *
get_string_prop (GDBusConnection *conn,
                 const gchar     *object_path,
                 const gchar     *iface,
                 const gchar     *prop)
{
    GError   *err = NULL;
    GVariant *ret = g_dbus_connection_call_sync (
        conn, UD_BUS_NAME, object_path,
        "org.freedesktop.DBus.Properties", "Get",
        g_variant_new ("(ss)", iface, prop),
        G_VARIANT_TYPE ("(v)"),
        G_DBUS_CALL_FLAGS_NONE, 2000, NULL, &err);
    if (!ret) { g_clear_error (&err); return g_strdup (""); }

    GVariant *val;
    g_variant_get (ret, "(v)", &val);

    gchar *str = NULL;
    if (g_variant_is_of_type (val, G_VARIANT_TYPE_STRING) ||
        g_variant_is_of_type (val, G_VARIANT_TYPE_OBJECT_PATH)) {
        str = g_variant_dup_string (val, NULL);
    } else if (g_variant_is_of_type (val, G_VARIANT_TYPE ("ay"))) {
        gsize len;
        const guchar *bytes = g_variant_get_fixed_array (val, &len, 1);
        if (bytes && len > 0) {
            if (bytes[len - 1] == '\0') len--;
            str = g_strndup ((const gchar *) bytes, len);
        }
    }

    g_variant_unref (val);
    g_variant_unref (ret);
    return str ? str : g_strdup ("");
}

static guint64
get_uint64_prop (GDBusConnection *conn,
                 const gchar     *object_path,
                 const gchar     *iface,
                 const gchar     *prop)
{
    GError   *err = NULL;
    GVariant *ret = g_dbus_connection_call_sync (
        conn, UD_BUS_NAME, object_path,
        "org.freedesktop.DBus.Properties", "Get",
        g_variant_new ("(ss)", iface, prop),
        G_VARIANT_TYPE ("(v)"),
        G_DBUS_CALL_FLAGS_NONE, 2000, NULL, &err);
    if (!ret) { g_clear_error (&err); return 0; }
    GVariant *val;
    g_variant_get (ret, "(v)", &val);
    guint64 n = g_variant_get_uint64 (val);
    g_variant_unref (val);
    g_variant_unref (ret);
    return n;
}

static gboolean
get_bool_prop (GDBusConnection *conn,
               const gchar     *object_path,
               const gchar     *iface,
               const gchar     *prop)
{
    GError   *err = NULL;
    GVariant *ret = g_dbus_connection_call_sync (
        conn, UD_BUS_NAME, object_path,
        "org.freedesktop.DBus.Properties", "Get",
        g_variant_new ("(ss)", iface, prop),
        G_VARIANT_TYPE ("(v)"),
        G_DBUS_CALL_FLAGS_NONE, 2000, NULL, &err);
    if (!ret) { g_clear_error (&err); return FALSE; }
    GVariant *val;
    g_variant_get (ret, "(v)", &val);
    gboolean b = g_variant_get_boolean (val);
    g_variant_unref (val);
    g_variant_unref (ret);
    return b;
}

static gchar *
get_mount_point (GDBusConnection *conn, const gchar *object_path)
{
    GError   *err = NULL;
    GVariant *ret = g_dbus_connection_call_sync (
        conn, UD_BUS_NAME, object_path,
        "org.freedesktop.DBus.Properties", "Get",
        g_variant_new ("(ss)", UD_FILESYSTEM_IFACE, "MountPoints"),
        G_VARIANT_TYPE ("(v)"),
        G_DBUS_CALL_FLAGS_NONE, 2000, NULL, &err);
    if (!ret) { g_clear_error (&err); return NULL; }

    GVariant *val;
    g_variant_get (ret, "(v)", &val);

    gchar *result = NULL;
    GVariantIter iter;
    g_variant_iter_init (&iter, val);
    GVariant *entry;
    if ((entry = g_variant_iter_next_value (&iter))) {
        gsize len;
        const guchar *bytes = g_variant_get_fixed_array (entry, &len, 1);
        if (bytes && len > 0)
            result = g_strndup ((const gchar *) bytes, len);
        g_variant_unref (entry);
    }

    g_variant_unref (val);
    g_variant_unref (ret);
    return result;
}

/* ---------- ud_get_devices ---------- */

GSList *
ud_get_devices (GDBusConnection *conn)
{
    GError   *err = NULL;
    GVariant *ret = g_dbus_connection_call_sync (
        conn, UD_BUS_NAME, UD_OBJECT_PATH,
        "org.freedesktop.DBus.ObjectManager", "GetManagedObjects",
        NULL, G_VARIANT_TYPE ("(a{oa{sa{sv}}})"),
        G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &err);

    if (!ret) {
        g_warning ("ud_get_devices: %s", err ? err->message : "error");
        g_clear_error (&err);
        return NULL;
    }

    GSList       *list = NULL;
    GVariant     *objects;
    GVariantIter  obj_iter;
    gchar        *obj_path;
    GVariant     *ifaces;

    g_variant_get (ret, "(@a{oa{sa{sv}}})", &objects);
    g_variant_iter_init (&obj_iter, objects);

    while (g_variant_iter_next (&obj_iter, "{o@a{sa{sv}}}", &obj_path, &ifaces)) {
        if (!g_str_has_prefix (obj_path, UD_OBJECT_PATH "/block_devices/")) {
            g_free (obj_path);
            g_variant_unref (ifaces);
            continue;
        }

        gboolean has_block = FALSE;
        gboolean has_fs    = FALSE;
        GVariantIter iface_iter;
        gchar *iface_name;
        GVariant *iface_props;
        g_variant_iter_init (&iface_iter, ifaces);
        while (g_variant_iter_next (&iface_iter, "{s@a{sv}}", &iface_name, &iface_props)) {
            if (g_strcmp0 (iface_name, UD_BLOCK_IFACE) == 0)      has_block = TRUE;
            if (g_strcmp0 (iface_name, UD_FILESYSTEM_IFACE) == 0) has_fs    = TRUE;
            g_free (iface_name);
            g_variant_unref (iface_props);
        }

        if (!has_block || !has_fs) {
            g_free (obj_path);
            g_variant_unref (ifaces);
            continue;
        }

        gboolean hint_ignore = get_bool_prop (conn, obj_path, UD_BLOCK_IFACE, "HintIgnore");
        gboolean hint_system = get_bool_prop (conn, obj_path, UD_BLOCK_IFACE, "HintSystem");
        if (hint_ignore || hint_system) {
            g_free (obj_path);
            g_variant_unref (ifaces);
            continue;
        }

        gchar *drive_path = get_string_prop (conn, obj_path, UD_BLOCK_IFACE, "Drive");
        if (!drive_path || g_strcmp0 (drive_path, "/") == 0) {
            g_free (drive_path);
            g_free (obj_path);
            g_variant_unref (ifaces);
            continue;
        }

        gboolean removable = get_bool_prop (conn, drive_path, UD_DRIVE_IFACE, "Removable");
        if (!removable) {
            g_free (drive_path);
            g_free (obj_path);
            g_variant_unref (ifaces);
            continue;
        }

        UdDevice *dev    = g_new0 (UdDevice, 1);
        dev->object_path = obj_path;
        dev->drive_path  = drive_path;
        dev->device      = get_string_prop (conn, obj_path, UD_BLOCK_IFACE, "Device");
        dev->label       = get_string_prop (conn, obj_path, UD_BLOCK_IFACE, "IdLabel");
        dev->size        = get_uint64_prop  (conn, obj_path, UD_BLOCK_IFACE, "Size");
        dev->ejectable   = get_bool_prop    (conn, drive_path, UD_DRIVE_IFACE, "Ejectable");
        dev->mount_point = get_mount_point  (conn, obj_path);
        dev->mounted     = (dev->mount_point != NULL);

        list = g_slist_append (list, dev);
        g_variant_unref (ifaces);
    }

    g_variant_unref (objects);
    g_variant_unref (ret);
    return list;
}

void
ud_device_list_free (GSList *list)
{
    for (GSList *l = list; l; l = l->next) {
        UdDevice *dev = l->data;
        g_free (dev->object_path);
        g_free (dev->device);
        g_free (dev->label);
        g_free (dev->drive_path);
        g_free (dev->mount_point);
        g_free (dev);
    }
    g_slist_free (list);
}

/* ---------- mount / unmount / eject ---------- */

gchar *
ud_mount (GDBusConnection *conn, const gchar *object_path)
{
    GError   *err = NULL;
    GVariant *ret = g_dbus_connection_call_sync (
        conn, UD_BUS_NAME, object_path,
        UD_FILESYSTEM_IFACE, "Mount",
        g_variant_new ("(a{sv})", NULL),
        G_VARIANT_TYPE ("(s)"),
        G_DBUS_CALL_FLAGS_NONE, 10000, NULL, &err);
    if (!ret) {
        g_warning ("ud_mount: %s", err ? err->message : "error");
        g_clear_error (&err);
        return NULL;
    }
    gchar *mp;
    g_variant_get (ret, "(s)", &mp);
    g_variant_unref (ret);
    return mp;
}

gboolean
ud_unmount (GDBusConnection *conn, const gchar *object_path)
{
    GError   *err = NULL;
    GVariant *ret = g_dbus_connection_call_sync (
        conn, UD_BUS_NAME, object_path,
        UD_FILESYSTEM_IFACE, "Unmount",
        g_variant_new ("(a{sv})", NULL),
        G_VARIANT_TYPE ("()"),
        G_DBUS_CALL_FLAGS_NONE, 10000, NULL, &err);
    if (!ret) {
        g_warning ("ud_unmount: %s", err ? err->message : "error");
        g_clear_error (&err);
        return FALSE;
    }
    g_variant_unref (ret);
    return TRUE;
}

gboolean
ud_eject (GDBusConnection *conn, const gchar *drive_path)
{
    GError   *err = NULL;
    GVariant *ret = g_dbus_connection_call_sync (
        conn, UD_BUS_NAME, drive_path,
        UD_DRIVE_IFACE, "Eject",
        g_variant_new ("(a{sv})", NULL),
        G_VARIANT_TYPE ("()"),
        G_DBUS_CALL_FLAGS_NONE, 10000, NULL, &err);
    if (!ret) {
        g_warning ("ud_eject: %s", err ? err->message : "error");
        g_clear_error (&err);
        return FALSE;
    }
    g_variant_unref (ret);
    return TRUE;
}

/* ---------- nombre del drive (Vendor + Model) ---------- */

gchar *
ud_get_drive_name (GDBusConnection *conn, const gchar *drive_path)
{
    gchar *vendor = get_string_prop (conn, drive_path, UD_DRIVE_IFACE, "Vendor");
    gchar *model  = get_string_prop (conn, drive_path, UD_DRIVE_IFACE, "Model");
    gchar *name;

    if (vendor && *vendor && model && *model)
        name = g_strdup_printf ("%s %s", vendor, model);
    else if (model && *model)
        name = g_strdup (model);
    else if (vendor && *vendor)
        name = g_strdup (vendor);
    else
        name = g_strdup ("");

    g_free (vendor);
    g_free (model);
    return name;
}

/* ---------- señales ---------- */

struct UdSubscription {
    UdSignalCallback callback;
    gpointer         user_data;
    GUdevClient     *udev_client;
    guint            debounce_id;
    guint            ids[3];
};

static gboolean
on_debounce_fire (gpointer data)
{
    UdSubscription *sd = data;
    sd->debounce_id = 0;
    sd->callback (sd->user_data);
    return G_SOURCE_REMOVE;
}

static void
on_udev_uevent (GUdevClient *client, const gchar *action,
                GUdevDevice *device, gpointer user_data)
{
    (void) client; (void) device;
    UdSubscription *sd = user_data;

    if (g_strcmp0 (action, "add") != 0 && g_strcmp0 (action, "remove") != 0)
        return;

    /* debounce 300ms: un solo enchufado dispara varios uevents (uno por interfaz) */
    if (sd->debounce_id != 0)
        g_source_remove (sd->debounce_id);
    sd->debounce_id = g_timeout_add (300, on_debounce_fire, sd);
}

static void
on_interfaces_changed (GDBusConnection *conn,
                       const gchar *sender, const gchar *object_path,
                       const gchar *iface, const gchar *signal,
                       GVariant *params, gpointer user_data)
{
    (void) conn; (void) sender; (void) object_path;
    (void) iface; (void) signal; (void) params;
    UdSubscription *sd = user_data;
    sd->callback (sd->user_data);
}

static void
on_properties_changed (GDBusConnection *conn,
                       const gchar *sender, const gchar *object_path,
                       const gchar *iface, const gchar *signal,
                       GVariant *params, gpointer user_data)
{
    (void) conn; (void) sender; (void) object_path;
    (void) iface; (void) signal; (void) params;
    UdSubscription *sd = user_data;
    sd->callback (sd->user_data);
}

UdSubscription *
ud_subscribe (GDBusConnection *conn,
              UdSignalCallback callback,
              gpointer         user_data)
{
    UdSubscription *sd = g_new0 (UdSubscription, 1);
    sd->callback  = callback;
    sd->user_data = user_data;

    /* monitor de uevents para detectar USB no-almacenamiento */
    const gchar *subsystems[] = { "usb", NULL };
    sd->udev_client = g_udev_client_new (subsystems);
    g_signal_connect (sd->udev_client, "uevent",
                      G_CALLBACK (on_udev_uevent), sd);

    sd->ids[0] = g_dbus_connection_signal_subscribe (
        conn, UD_BUS_NAME,
        "org.freedesktop.DBus.ObjectManager", "InterfacesAdded",
        UD_OBJECT_PATH, NULL, G_DBUS_SIGNAL_FLAGS_NONE,
        on_interfaces_changed, sd, NULL);

    sd->ids[1] = g_dbus_connection_signal_subscribe (
        conn, UD_BUS_NAME,
        "org.freedesktop.DBus.ObjectManager", "InterfacesRemoved",
        UD_OBJECT_PATH, NULL, G_DBUS_SIGNAL_FLAGS_NONE,
        on_interfaces_changed, sd, NULL);

    sd->ids[2] = g_dbus_connection_signal_subscribe (
        conn, UD_BUS_NAME,
        "org.freedesktop.DBus.Properties", "PropertiesChanged",
        NULL, UD_FILESYSTEM_IFACE, G_DBUS_SIGNAL_FLAGS_NONE,
        on_properties_changed, sd, NULL);

    return sd;
}

void
ud_unsubscribe (GDBusConnection *conn, UdSubscription *sub)
{
    if (!sub) return;
    for (gint i = 0; i < 3; i++)
        if (sub->ids[i] != 0)
            g_dbus_connection_signal_unsubscribe (conn, sub->ids[i]);
    if (sub->debounce_id != 0)
        g_source_remove (sub->debounce_id);
    if (sub->udev_client)
        g_object_unref (sub->udev_client);
    g_free (sub);
}

/* ---------- ud_get_usb_devices ---------- */

/*
 * Estrategia: iterar subsistema "usb" buscando usb_device,
 * filtrar hubs (clase 09) y almacenamiento (usb-storage/uas),
 * deduplicar por ID_SERIAL, estimar tipo por subsistema hijo o clase USB.
 */
GSList *
ud_get_usb_devices (void)
{
    const gchar *subsystems[] = { "usb", NULL };
    GUdevClient *client = g_udev_client_new (subsystems);
    GSList      *result = NULL;
    GHashTable  *seen   = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

    GList *usb_devs = g_udev_client_query_by_subsystem (client, "usb");
    for (GList *l = usb_devs; l; l = l->next) {
        GUdevDevice *dev = l->data;

        /* solo usb_device */
        const gchar *devtype = g_udev_device_get_devtype (dev);
        if (g_strcmp0 (devtype, "usb_device") != 0) {
            g_object_unref (dev);
            continue;
        }

        /* filtrar hubs (clase 09) */
        const gchar *bclass = g_udev_device_get_sysfs_attr (dev, "bDeviceClass");
        if (g_strcmp0 (bclass, "09") == 0) {
            g_object_unref (dev);
            continue;
        }

        /* filtrar almacenamiento — ya aparece en la sección principal */
        const gchar *driver = g_udev_device_get_driver (dev);
        if (g_strcmp0 (driver, "usb-storage") == 0 ||
            g_strcmp0 (driver, "uas") == 0) {
            g_object_unref (dev);
            continue;
        }

        /* deduplicar por ID_SERIAL */
        const gchar *serial = g_udev_device_get_property (dev, "ID_SERIAL");
        if (!serial || !*serial)
            serial = g_udev_device_get_property (dev, "ID_PATH");
        if (!serial || !*serial) { g_object_unref (dev); continue; }
        if (g_hash_table_contains (seen, serial)) {
            g_object_unref (dev);
            continue;
        }
        g_hash_table_insert (seen, g_strdup (serial), GINT_TO_POINTER (1));

        /* nombre */
        const gchar *vendor = g_udev_device_get_property (dev, "ID_VENDOR_FROM_DATABASE");
        if (!vendor || !*vendor) vendor = g_udev_device_get_property (dev, "ID_VENDOR");
        const gchar *model  = g_udev_device_get_property (dev, "ID_MODEL_FROM_DATABASE");
        if (!model  || !*model)  model  = g_udev_device_get_property (dev, "ID_MODEL");

        gchar *name = NULL;
        if (vendor && model && *vendor && *model)
            name = g_strdup_printf ("%s %s", vendor, model);
        else if (model && *model)
            name = g_strdup (model);
        else if (vendor && *vendor)
            name = g_strdup (vendor);
        else {
            const gchar *prod = g_udev_device_get_sysfs_attr (dev, "product");
            name = (prod && *prod) ? g_strdup (prod) : g_strdup ("Unknown USB device");
        }
        /* reemplazar guiones bajos por espacios */
        for (gchar *c = name; *c; c++) if (*c == '_') *c = ' ';

        /* leer interfaces del dispositivo desde sysfs */
        gchar *sysfs_path = g_strdup (g_udev_device_get_sysfs_path (dev));
        gchar *iface_glob = g_strdup_printf ("%s/*/bInterfaceClass", sysfs_path);
        GDir  *sysfs_dir  = g_dir_open (sysfs_path, 0, NULL);
        gboolean has_hid = FALSE, has_vendor = FALSE, has_printer = FALSE;
        gboolean has_audio = FALSE, has_video = FALSE, has_bt = FALSE;
        gint     hid_count = 0;

        if (sysfs_dir) {
            const gchar *entry;
            while ((entry = g_dir_read_name (sysfs_dir))) {
                gchar *iface_path = g_strdup_printf ("%s/%s/bInterfaceClass", sysfs_path, entry);
                gchar *iface_class = NULL;
                if (g_file_get_contents (iface_path, &iface_class, NULL, NULL)) {
                    g_strstrip (iface_class);
                    if (g_strcmp0 (iface_class, "03") == 0) { has_hid = TRUE; hid_count++; }
                    if (g_strcmp0 (iface_class, "ff") == 0) has_vendor = TRUE;
                    if (g_strcmp0 (iface_class, "07") == 0) has_printer = TRUE;
                    if (g_strcmp0 (iface_class, "01") == 0) has_audio = TRUE;
                    if (g_strcmp0 (iface_class, "0e") == 0) has_video = TRUE;
                    if (g_strcmp0 (iface_class, "e0") == 0) has_bt = TRUE;
                    g_free (iface_class);
                }
                g_free (iface_path);
            }
            g_dir_close (sysfs_dir);
        }
        g_free (sysfs_path);
        g_free (iface_glob);

        /* filtrar: HID puro sin vendor = teclado/mouse/receptor → excluir */
        if (has_hid && !has_vendor && !has_printer && !has_audio && !has_video) {
            g_object_unref (dev);
            continue;
        }

        /* filtrar: vendor puro sin HID = WiFi/otros → excluir */
        if (has_vendor && !has_hid && !has_printer && !has_audio && !has_video) {
            g_object_unref (dev);
            continue;
        }

        /* filtrar: Bluetooth (clase e0) → excluir */
        if (has_bt && !has_printer && !has_audio && !has_video && !has_hid) {
            g_object_unref (dev);
            continue;
        }

        /* tipo */
        gchar *type = NULL;
        if (has_printer)                type = g_strdup ("Printer");
        else if (has_audio)             type = g_strdup ("Audio device");
        else if (has_video)             type = g_strdup ("Camera / Webcam");
        else if (has_hid && has_vendor) type = g_strdup ("Gamepad / Joystick");
        else                            type = g_strdup ("USB device");

        UdUsbDevice *ud = g_new0 (UdUsbDevice, 1);
        ud->name    = name;
        ud->type    = type;
        ud->syspath = g_strdup (g_udev_device_get_sysfs_path (dev));
        result = g_slist_append (result, ud);

        g_object_unref (dev);
    }

    g_list_free (usb_devs);
    g_hash_table_destroy (seen);
    g_object_unref (client);
    return result;
}

void
ud_usb_device_list_free (GSList *list)
{
    for (GSList *l = list; l; l = l->next) {
        UdUsbDevice *dev = l->data;
        g_free (dev->name);
        g_free (dev->type);
        g_free (dev->syspath);
        g_free (dev);
    }
    g_slist_free (list);
}
