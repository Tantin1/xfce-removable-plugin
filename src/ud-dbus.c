#include "ud-dbus.h"
#include <string.h>
#include <gudev/gudev.h>

/* marca cadenas para xgettext; se traducen con _() al mostrarlas en la UI */
#define N_(x) (x)

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

/* ---------- lectura de propiedades desde GetManagedObjects ----------
 *
 * GetManagedObjects ya trae todas las interfaces y propiedades de todos los
 * objetos (bloques y drives) en una sola llamada: no hace falta volver a
 * consultar el bus por cada propiedad.
 */

/* convierte un "ay" de UDisks2 (terminado en NUL) a string */
static gchar *
bytes_to_str (GVariant *v)
{
    gsize        len   = 0;
    const gchar *bytes = g_variant_get_fixed_array (v, &len, 1);
    while (len > 0 && bytes[len - 1] == '\0')
        len--;
    return (len > 0) ? g_strndup (bytes, len) : NULL;
}

static gchar *
prop_str (GVariant *props, const gchar *key)
{
    gchar    *str = NULL;
    GVariant *v   = props ? g_variant_lookup_value (props, key, NULL) : NULL;
    if (v) {
        if (g_variant_is_of_type (v, G_VARIANT_TYPE_STRING) ||
            g_variant_is_of_type (v, G_VARIANT_TYPE_OBJECT_PATH))
            str = g_variant_dup_string (v, NULL);
        else if (g_variant_is_of_type (v, G_VARIANT_TYPE_BYTESTRING))
            str = bytes_to_str (v);
        g_variant_unref (v);
    }
    return str ? str : g_strdup ("");
}

static gboolean
prop_bool (GVariant *props, const gchar *key)
{
    gboolean b = FALSE;
    if (props)
        g_variant_lookup (props, key, "b", &b);
    return b;
}

static guint64
prop_u64 (GVariant *props, const gchar *key)
{
    guint64 n = 0;
    if (props)
        g_variant_lookup (props, key, "t", &n);
    return n;
}

static gchar *
prop_first_mount_point (GVariant *fs_props)
{
    gchar    *mp = NULL;
    GVariant *v  = g_variant_lookup_value (fs_props, "MountPoints", G_VARIANT_TYPE ("aay"));
    if (v) {
        if (g_variant_n_children (v) > 0) {
            GVariant *first = g_variant_get_child_value (v, 0);
            mp = bytes_to_str (first);
            g_variant_unref (first);
        }
        g_variant_unref (v);
    }
    return mp;
}

static gchar *
drive_display_name (GVariant *drive_props)
{
    gchar *vendor = g_strstrip (prop_str (drive_props, "Vendor"));
    gchar *model  = g_strstrip (prop_str (drive_props, "Model"));
    gchar *name;

    if (*vendor && *model) name = g_strdup_printf ("%s %s", vendor, model);
    else if (*model)       name = g_strdup (model);
    else                   name = g_strdup (vendor);

    g_free (vendor);
    g_free (model);
    return name;
}

/* devuelve un UdDevice si el objeto es un filesystem en un drive extraíble */
static UdDevice *
parse_block (const gchar *path, GVariant *ifaces, GHashTable *drives)
{
    UdDevice *dev   = NULL;
    GVariant *block = g_variant_lookup_value (ifaces, UD_BLOCK_IFACE,      G_VARIANT_TYPE_VARDICT);
    GVariant *fs    = g_variant_lookup_value (ifaces, UD_FILESYSTEM_IFACE, G_VARIANT_TYPE_VARDICT);
    GVariant *part  = g_variant_lookup_value (ifaces, UD_PARTITION_IFACE,  G_VARIANT_TYPE_VARDICT);

    if (block && fs &&
        !prop_bool (block, "HintIgnore") && !prop_bool (block, "HintSystem")) {
        gchar    *drive_path = prop_str (block, "Drive");
        GVariant *drive      = g_hash_table_lookup (drives, drive_path);

        if (drive && prop_bool (drive, "Removable")) {
            dev                = g_new0 (UdDevice, 1);
            dev->object_path   = g_strdup (path);
            dev->drive_path    = drive_path;
            drive_path         = NULL;
            dev->device        = prop_str (block, "Device");
            dev->label         = prop_str (block, "IdLabel");
            dev->fs_type       = prop_str (block, "IdType");
            dev->size          = prop_u64 (block, "Size");
            dev->mount_point   = prop_first_mount_point (fs);
            dev->mounted       = (dev->mount_point != NULL);
            dev->is_partition  = (part != NULL);
            dev->drive_name    = drive_display_name (drive);
            dev->ejectable     = prop_bool (drive, "Ejectable");
            dev->can_power_off = prop_bool (drive, "CanPowerOff");
        }
        g_free (drive_path);
    }

    if (block) g_variant_unref (block);
    if (fs)    g_variant_unref (fs);
    if (part)  g_variant_unref (part);
    return dev;
}

static gint
compare_devices (gconstpointer a, gconstpointer b)
{
    const UdDevice *da = a, *db = b;
    gint c = g_strcmp0 (da->drive_path, db->drive_path);
    return (c != 0) ? c : g_strcmp0 (da->device, db->device);
}

/* ---------- ud_get_devices ---------- */

GSList *
ud_get_devices (GDBusConnection *conn)
{
    if (!conn)
        return NULL;

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

    GVariant    *objects = g_variant_get_child_value (ret, 0);
    GHashTable  *drives  = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                                  (GDestroyNotify) g_variant_unref);
    GSList      *list    = NULL;
    GVariantIter iter;
    const gchar *path;
    GVariant    *ifaces;

    /* 1ª pasada: propiedades de los drives */
    g_variant_iter_init (&iter, objects);
    while (g_variant_iter_next (&iter, "{&o@a{sa{sv}}}", &path, &ifaces)) {
        GVariant *drive = g_variant_lookup_value (ifaces, UD_DRIVE_IFACE, G_VARIANT_TYPE_VARDICT);
        if (drive)
            g_hash_table_insert (drives, g_strdup (path), drive);
        g_variant_unref (ifaces);
    }

    /* 2ª pasada: bloques con filesystem */
    g_variant_iter_init (&iter, objects);
    while (g_variant_iter_next (&iter, "{&o@a{sa{sv}}}", &path, &ifaces)) {
        UdDevice *dev = parse_block (path, ifaces, drives);
        if (dev)
            list = g_slist_prepend (list, dev);
        g_variant_unref (ifaces);
    }

    g_hash_table_destroy (drives);
    g_variant_unref (objects);
    g_variant_unref (ret);

    /* orden estable entre refrescos (GetManagedObjects no garantiza orden) */
    return g_slist_sort (list, compare_devices);
}

void
ud_device_list_free (GSList *list)
{
    for (GSList *l = list; l; l = l->next) {
        UdDevice *dev = l->data;
        g_free (dev->object_path);
        g_free (dev->device);
        g_free (dev->label);
        g_free (dev->fs_type);
        g_free (dev->drive_path);
        g_free (dev->drive_name);
        g_free (dev->mount_point);
        g_free (dev);
    }
    g_slist_free (list);
}

/* ---------- mount / unmount / eject / power off asíncronos ---------- */

typedef struct {
    UdResultCallback callback;
    gpointer         user_data;
    gboolean         returns_string;
} UdAsyncOp;

static void
on_async_call_done (GObject *source, GAsyncResult *res, gpointer data)
{
    UdAsyncOp *op  = data;
    GError    *err = NULL;
    gchar     *str = NULL;
    GVariant  *ret = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), res, &err);
    gboolean   ok  = (ret != NULL);

    if (ok) {
        if (op->returns_string)
            g_variant_get (ret, "(s)", &str);
        g_variant_unref (ret);
    } else {
        g_warning ("ud-dbus: %s", err ? err->message : "error");
    }

    if (op->callback)
        op->callback (ok, str, err, op->user_data);

    g_free (str);
    g_clear_error (&err);
    g_free (op);
}

static void
ud_call_async (GDBusConnection    *conn,
               const gchar        *object_path,
               const gchar        *iface,
               const gchar        *method,
               const GVariantType *reply_type,
               gboolean            returns_string,
               UdResultCallback    callback,
               gpointer            user_data)
{
    UdAsyncOp *op      = g_new0 (UdAsyncOp, 1);
    op->callback       = callback;
    op->user_data      = user_data;
    op->returns_string = returns_string;

    /* ALLOW_INTERACTIVE_AUTHORIZATION: deja que polkit pida contraseña si hace falta.
       Timeout G_MAXINT: el usuario puede tardar en escribirla, y un desmontaje
       con caché sucio puede demorar; al ser asíncrono no bloquea nada. */
    g_dbus_connection_call (conn, UD_BUS_NAME, object_path, iface, method,
                            g_variant_new ("(a{sv})", NULL),
                            reply_type,
                            G_DBUS_CALL_FLAGS_ALLOW_INTERACTIVE_AUTHORIZATION,
                            G_MAXINT, NULL,
                            on_async_call_done, op);
}

void
ud_mount_async (GDBusConnection *conn, const gchar *object_path,
                UdResultCallback callback, gpointer user_data)
{
    ud_call_async (conn, object_path, UD_FILESYSTEM_IFACE, "Mount",
                   G_VARIANT_TYPE ("(s)"), TRUE, callback, user_data);
}

void
ud_unmount_async (GDBusConnection *conn, const gchar *object_path,
                  UdResultCallback callback, gpointer user_data)
{
    ud_call_async (conn, object_path, UD_FILESYSTEM_IFACE, "Unmount",
                   G_VARIANT_TYPE ("()"), FALSE, callback, user_data);
}

void
ud_eject_async (GDBusConnection *conn, const gchar *drive_path,
                UdResultCallback callback, gpointer user_data)
{
    ud_call_async (conn, drive_path, UD_DRIVE_IFACE, "Eject",
                   G_VARIANT_TYPE ("()"), FALSE, callback, user_data);
}

void
ud_power_off_async (GDBusConnection *conn, const gchar *drive_path,
                    UdResultCallback callback, gpointer user_data)
{
    ud_call_async (conn, drive_path, UD_DRIVE_IFACE, "PowerOff",
                   G_VARIANT_TYPE ("()"), FALSE, callback, user_data);
}

/* ---------- señales ---------- */

struct UdSubscription {
    UdSignalCallback callback;
    gpointer         user_data;
    GUdevClient     *udev_client;
    GVolumeMonitor  *volume_monitor;   /* teléfonos/cámaras vía gvfs */
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

/* debounce 300ms para TODAS las fuentes: un enchufado dispara varios uevents
   (uno por interfaz USB) y varios InterfacesAdded (uno por partición) */
static void
schedule_refresh (UdSubscription *sd)
{
    if (sd->debounce_id != 0)
        g_source_remove (sd->debounce_id);
    sd->debounce_id = g_timeout_add (300, on_debounce_fire, sd);
}

static void
on_udev_uevent (GUdevClient *client, const gchar *action,
                GUdevDevice *device, gpointer user_data)
{
    (void) client; (void) device;
    if (g_strcmp0 (action, "add") != 0 && g_strcmp0 (action, "remove") != 0)
        return;
    schedule_refresh (user_data);
}

static void
on_dbus_signal (GDBusConnection *conn,
                const gchar *sender, const gchar *object_path,
                const gchar *iface, const gchar *signal,
                GVariant *params, gpointer user_data)
{
    (void) conn; (void) sender; (void) object_path;
    (void) iface; (void) signal; (void) params;
    schedule_refresh (user_data);
}

/* volume-added/removed/changed y mount-added/removed/changed:
   todas son (GVolumeMonitor *, GObject *, gpointer) */
static void
on_volume_monitor_changed (GVolumeMonitor *vm, GObject *obj, gpointer user_data)
{
    (void) vm; (void) obj;
    schedule_refresh (user_data);
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

    /* singleton por proceso (en Wayland lo comparten todos los plugins):
       solo se conectan señales con nuestro dato y se desconectan al salir */
    sd->volume_monitor = g_volume_monitor_get ();
    static const gchar *vm_signals[] = {
        "volume-added", "volume-removed", "volume-changed",
        "mount-added",  "mount-removed",  "mount-changed",
    };
    for (gsize i = 0; i < G_N_ELEMENTS (vm_signals); i++)
        g_signal_connect (sd->volume_monitor, vm_signals[i],
                          G_CALLBACK (on_volume_monitor_changed), sd);

    sd->ids[0] = g_dbus_connection_signal_subscribe (
        conn, UD_BUS_NAME,
        "org.freedesktop.DBus.ObjectManager", "InterfacesAdded",
        UD_OBJECT_PATH, NULL, G_DBUS_SIGNAL_FLAGS_NONE,
        on_dbus_signal, sd, NULL);

    sd->ids[1] = g_dbus_connection_signal_subscribe (
        conn, UD_BUS_NAME,
        "org.freedesktop.DBus.ObjectManager", "InterfacesRemoved",
        UD_OBJECT_PATH, NULL, G_DBUS_SIGNAL_FLAGS_NONE,
        on_dbus_signal, sd, NULL);

    sd->ids[2] = g_dbus_connection_signal_subscribe (
        conn, UD_BUS_NAME,
        "org.freedesktop.DBus.Properties", "PropertiesChanged",
        NULL, UD_FILESYSTEM_IFACE, G_DBUS_SIGNAL_FLAGS_NONE,
        on_dbus_signal, sd, NULL);

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
    if (sub->volume_monitor) {
        g_signal_handlers_disconnect_by_data (sub->volume_monitor, sub);
        g_object_unref (sub->volume_monitor);
    }
    g_free (sub);
}

/* ---------- tipos de USB no-almacenamiento ---------- */

const gchar *
ud_usb_type_label (UdUsbType type)
{
    switch (type) {
    case UD_USB_PRINTER: return N_("Printer");
    case UD_USB_AUDIO:   return N_("Audio device");
    case UD_USB_CAMERA:  return N_("Camera / Webcam");
    case UD_USB_GAMEPAD: return N_("Gamepad / Joystick");
    case UD_USB_PHONE:   return N_("Phone / Camera");
    case UD_USB_OTHER:
    default:             return N_("USB device");
    }
}

const gchar *
ud_usb_type_icon (UdUsbType type)
{
    switch (type) {
    case UD_USB_PRINTER: return "printer";
    case UD_USB_AUDIO:   return "audio-card";
    case UD_USB_CAMERA:  return "camera-web";
    case UD_USB_GAMEPAD: return "input-gaming";
    case UD_USB_PHONE:   return "phone";
    case UD_USB_OTHER:
    default:             return "drive-removable-media";
    }
}

/* ---------- ud_get_usb_devices ---------- */

/*
 * Estrategia: iterar subsistema "usb" buscando usb_device,
 * filtrar hubs (clase 09) y almacenamiento (usb-storage/uas),
 * deduplicar por ID_SERIAL, estimar tipo por clase de interfaz USB.
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
        const gchar *sysfs_path = g_udev_device_get_sysfs_path (dev);
        GDir        *sysfs_dir  = g_dir_open (sysfs_path, 0, NULL);
        gboolean has_hid = FALSE, has_vendor = FALSE, has_printer = FALSE;
        gboolean has_audio = FALSE, has_video = FALSE, has_bt = FALSE;
        gboolean has_image = FALSE;   /* 06: Still Image (PTP / MTP de Android) */

        if (sysfs_dir) {
            const gchar *entry;
            while ((entry = g_dir_read_name (sysfs_dir))) {
                gchar *iface_path = g_strdup_printf ("%s/%s/bInterfaceClass", sysfs_path, entry);
                gchar *iface_class = NULL;
                if (g_file_get_contents (iface_path, &iface_class, NULL, NULL)) {
                    g_strstrip (iface_class);
                    if (g_strcmp0 (iface_class, "03") == 0) has_hid     = TRUE;
                    if (g_strcmp0 (iface_class, "ff") == 0) has_vendor  = TRUE;
                    if (g_strcmp0 (iface_class, "07") == 0) has_printer = TRUE;
                    if (g_strcmp0 (iface_class, "01") == 0) has_audio   = TRUE;
                    if (g_strcmp0 (iface_class, "0e") == 0) has_video   = TRUE;
                    if (g_strcmp0 (iface_class, "e0") == 0) has_bt      = TRUE;
                    if (g_strcmp0 (iface_class, "06") == 0) has_image   = TRUE;
                    g_free (iface_class);
                }
                g_free (iface_path);
            }
            g_dir_close (sysfs_dir);
        }

        /* las reglas udev de libmtp marcan los teléfonos MTP (aunque usen clase ff) */
        if (g_strcmp0 (g_udev_device_get_property (dev, "ID_MTP_DEVICE"), "1") == 0)
            has_image = TRUE;

        gboolean relevant = has_printer || has_audio || has_video || has_image;

        /* filtrar: HID puro sin vendor = teclado/mouse/receptor → excluir */
        /* filtrar: vendor puro sin HID = WiFi/otros → excluir */
        /* filtrar: Bluetooth (clase e0) → excluir */
        if ((has_hid && !has_vendor && !relevant) ||
            (has_vendor && !has_hid && !relevant) ||
            (has_bt && !has_hid && !relevant)) {
            g_free (name);
            g_object_unref (dev);
            continue;
        }

        UdUsbType type;
        if (has_image)                  type = UD_USB_PHONE;
        else if (has_printer)           type = UD_USB_PRINTER;
        else if (has_audio)             type = UD_USB_AUDIO;
        else if (has_video)             type = UD_USB_CAMERA;
        else if (has_hid && has_vendor) type = UD_USB_GAMEPAD;
        else                            type = UD_USB_OTHER;

        UdUsbDevice *ud = g_new0 (UdUsbDevice, 1);
        ud->name    = name;
        ud->type    = type;
        ud->syspath = g_strdup (sysfs_path);
        ud->devfile = g_strdup (g_udev_device_get_device_file (dev));
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
        g_free (dev->syspath);
        g_free (dev->devfile);
        g_free (dev);
    }
    g_slist_free (list);
}
