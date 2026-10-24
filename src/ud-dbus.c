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
    case UD_USB_PRINTER:  return N_("Printer");
    case UD_USB_AUDIO:    return N_("Audio device");
    case UD_USB_CAMERA:   return N_("Camera / Webcam");
    case UD_USB_GAMEPAD:  return N_("Gamepad / Joystick");
    case UD_USB_PHONE:    return N_("Phone / Camera");
    case UD_USB_TABLET:   return N_("Graphics tablet");
    case UD_USB_SERIAL:   return N_("Serial device / Board");
    case UD_USB_SECURITY: return N_("Security key / Smart card");
    case UD_USB_MIDI:     return N_("MIDI device");
    case UD_USB_OTHER:
    default:              return N_("USB device");
    }
}

const gchar *
ud_usb_type_icon (UdUsbType type)
{
    switch (type) {
    case UD_USB_PRINTER:  return "printer";
    case UD_USB_AUDIO:    return "audio-card";
    case UD_USB_CAMERA:   return "camera-web";
    case UD_USB_GAMEPAD:  return "input-gaming";
    case UD_USB_PHONE:    return "phone";
    case UD_USB_TABLET:   return "input-tablet";
    case UD_USB_SERIAL:   return "utilities-terminal";
    case UD_USB_SECURITY: return "dialog-password";
    case UD_USB_MIDI:     return "audio-x-generic";
    case UD_USB_OTHER:
    default:              return "drive-removable-media";
    }
}

/* ---------- ud_get_usb_devices ----------
 *
 * Por cada usb_device (no por interfaz):
 *
 * 1. Se descartan hubs (clase 09) y los puertos que el firmware declara
 *    internos (atributo sysfs "removable" == "fixed": webcam, lector de
 *    huellas o Bluetooth integrados de una notebook).
 * 2. Se juntan dos fuentes:
 *    - las interfaces USB (clase, subclase y driver de cada una, desde sysfs);
 *    - lo que udev ya clasificó en los dispositivos "hijos": input
 *      (ID_INPUT_JOYSTICK/TABLET/KEYBOARD/MOUSE/TOUCHPAD), hidraw
 *      (ID_SECURITY_TOKEN, de las reglas fido_id), net, bluetooth y tty.
 *      Las marcas de udev son más fiables que adivinar por clase de interfaz:
 *      los mandos de Xbox no son HID, los de PlayStation traen audio, etc.
 * 3. Se clasifica por prioridad (ver classify_usb()).
 *
 * No se deduplica: cada usb_device aparece una sola vez en la consulta, y
 * deduplicar por ID_SERIAL juntaba dos aparatos iguales sin número de serie.
 */

enum {
    CH_JOYSTICK = 1 << 0,
    CH_TABLET   = 1 << 1,
    CH_KBMOUSE  = 1 << 2,
    CH_SECURITY = 1 << 3,
    CH_NET      = 1 << 4,
    CH_BT       = 1 << 5,
    CH_TTY      = 1 << 6,
};

static guint
classify_input (GUdevDevice *d)
{
    if (g_udev_device_get_property_as_boolean (d, "ID_INPUT_JOYSTICK"))
        return CH_JOYSTICK;
    if (g_udev_device_get_property_as_boolean (d, "ID_INPUT_TABLET"))
        return CH_TABLET;
    if (g_udev_device_get_property_as_boolean (d, "ID_INPUT_KEYBOARD") ||
        g_udev_device_get_property_as_boolean (d, "ID_INPUT_MOUSE")    ||
        g_udev_device_get_property_as_boolean (d, "ID_INPUT_TOUCHPAD"))
        return CH_KBMOUSE;
    return 0;
}

static guint
classify_hidraw (GUdevDevice *d)
{
    return g_udev_device_get_property_as_boolean (d, "ID_SECURITY_TOKEN") ? CH_SECURITY : 0;
}

static guint classify_net (GUdevDevice *d) { (void) d; return CH_NET; }
static guint classify_bt  (GUdevDevice *d) { (void) d; return CH_BT;  }
static guint classify_tty (GUdevDevice *d) { (void) d; return CH_TTY; }

/* marca en `flags` (syspath del usb_device -> bits) lo que aportan los hijos
   de un subsistema */
static void
mark_children (GUdevClient *client, const gchar *subsystem,
               guint (*classify) (GUdevDevice *), GHashTable *flags)
{
    GList *devs = g_udev_client_query_by_subsystem (client, subsystem);
    for (GList *l = devs; l; l = l->next) {
        GUdevDevice *d    = l->data;
        guint        bits = classify (d);
        if (bits) {
            GUdevDevice *usb = g_udev_device_get_parent_with_subsystem (d, "usb", "usb_device");
            if (usb) {
                const gchar *key = g_udev_device_get_sysfs_path (usb);
                guint        old = GPOINTER_TO_UINT (g_hash_table_lookup (flags, key));
                g_hash_table_replace (flags, g_strdup (key), GUINT_TO_POINTER (old | bits));
                g_object_unref (usb);
            }
        }
        g_object_unref (d);
    }
    g_list_free (devs);
}

/* lo que dicen las interfaces USB del aparato */
typedef struct {
    gboolean hid, vendor, storage, printer, video, image, bt, smartcard;
    gboolean audio_stream;   /* 01/02: placa de audio, micrófono, auriculares */
    gboolean midi;           /* 01/03: MIDIStreaming */
    gboolean cdc_acm;        /* 02/02: módem / serie (Arduino) */
    gboolean serial_driver;  /* cdc_acm, ftdi_sio, ch341, cp210x, pl2303… */
    gboolean unknown_std;    /* alguna clase estándar sin categoría propia */
} UsbIfaces;

static gboolean
is_serial_driver (const gchar *drv)
{
    static const gchar *serial[] = {
        "cdc_acm", "ftdi_sio", "ch341", "cp210x", "pl2303", "usb_serial_simple",
        "ti_usb_3410_5052", "io_ti", "mos7840", "oti6858", "spcp8x5", "ark3116",
        "f81232", "xr_serial", NULL,
    };
    return drv && g_strv_contains (serial, drv);
}

static gchar *
read_sysfs_line (const gchar *dir, const gchar *entry, const gchar *file)
{
    gchar *path = g_build_filename (dir, entry, file, NULL);
    gchar *val  = NULL;
    if (g_file_get_contents (path, &val, NULL, NULL))
        g_strstrip (val);
    g_free (path);
    return val;
}

static void
read_interfaces (const gchar *sysfs_path, UsbIfaces *f)
{
    GDir        *dir = g_dir_open (sysfs_path, 0, NULL);
    const gchar *entry;

    if (!dir)
        return;
    while ((entry = g_dir_read_name (dir))) {
        gchar *cls = read_sysfs_line (sysfs_path, entry, "bInterfaceClass");
        if (!cls)
            continue;                       /* no es una interfaz */
        gchar *sub  = read_sysfs_line (sysfs_path, entry, "bInterfaceSubClass");
        gchar *link = g_build_filename (sysfs_path, entry, "driver", NULL);
        gchar *tgt  = g_file_read_link (link, NULL);
        gchar *drv  = tgt ? g_path_get_basename (tgt) : NULL;

        if      (!g_strcmp0 (cls, "03")) f->hid       = TRUE;
        else if (!g_strcmp0 (cls, "ff")) f->vendor    = TRUE;
        else if (!g_strcmp0 (cls, "08")) f->storage   = TRUE;
        else if (!g_strcmp0 (cls, "07")) f->printer   = TRUE;
        else if (!g_strcmp0 (cls, "0e")) f->video     = TRUE;
        else if (!g_strcmp0 (cls, "06")) f->image     = TRUE;
        else if (!g_strcmp0 (cls, "e0")) f->bt        = TRUE;
        else if (!g_strcmp0 (cls, "0b")) f->smartcard = TRUE;
        else if (!g_strcmp0 (cls, "01")) {
            if      (!g_strcmp0 (sub, "02")) f->audio_stream = TRUE;
            else if (!g_strcmp0 (sub, "03")) f->midi         = TRUE;
            /* 01/01 (AudioControl) solo acompaña a los otros dos */
        }
        else if (!g_strcmp0 (cls, "02")) {
            if (!g_strcmp0 (sub, "02")) f->cdc_acm = TRUE;
            /* 02/06, 02/0d… (Ethernet, NCM) los detecta CH_NET */
        }
        else if (!g_strcmp0 (cls, "0a")) { /* datos CDC: acompaña a la 02 */ }
        else if (!g_strcmp0 (cls, "ef")) { /* misceláneo / IAD: contenedor */ }
        else
            f->unknown_std = TRUE;

        if (is_serial_driver (drv))
            f->serial_driver = TRUE;
        /* almacenamiento: a veces la interfaz es ff con usb-storage/uas */
        if (!g_strcmp0 (drv, "usb-storage") || !g_strcmp0 (drv, "uas"))
            f->storage = TRUE;

        g_free (drv);
        g_free (tgt);
        g_free (link);
        g_free (sub);
        g_free (cls);
    }
    g_dir_close (dir);
}

/* devuelve TRUE y el tipo si el aparato va en "Otros dispositivos".
   El orden importa: cada regla solo mira lo que no resolvieron las anteriores. */
static gboolean
classify_usb (const UsbIfaces *f, guint ch, gboolean mtp, UdUsbType *type)
{
    /* lo que el usuario enchufa a propósito, de lo más específico a lo menos */
    if (f->image || mtp)              { *type = UD_USB_PHONE;    return TRUE; }
    if (ch & CH_JOYSTICK)             { *type = UD_USB_GAMEPAD;  return TRUE; }  /* antes que audio: DualSense */
    if (ch & CH_TABLET)               { *type = UD_USB_TABLET;   return TRUE; }  /* antes que mouse: Wacom */
    if ((ch & CH_SECURITY) || f->smartcard)
                                      { *type = UD_USB_SECURITY; return TRUE; }
    if (f->printer)                   { *type = UD_USB_PRINTER;  return TRUE; }
    if (f->video)                     { *type = UD_USB_CAMERA;   return TRUE; }  /* antes que audio: micrófono de la webcam */
    if (f->audio_stream)              { *type = UD_USB_AUDIO;    return TRUE; }
    if (f->midi)                      { *type = UD_USB_MIDI;     return TRUE; }

    /* van en otros plugins (red, Bluetooth) o en la sección principal */
    if ((ch & (CH_NET | CH_BT)) || f->bt)
        return FALSE;

    /* antes que teclado/mouse: un Arduino Leonardo emula teclado y además es serie */
    if ((ch & CH_TTY) || f->serial_driver || f->cdc_acm)
                                      { *type = UD_USB_SERIAL;   return TRUE; }

    if (ch & CH_KBMOUSE)              return FALSE;   /* teclados, mouses, receptores */
    if (f->storage)                   return FALSE;   /* ya está en la sección principal */

    /* sin categoría: solo si tiene alguna clase estándar reconocible. HID puro
       (RGB, sensores, botoneras) y "de fabricante" puro (huelleros, controladores
       varios) quedan afuera: casi siempre son internos o no le dicen nada al usuario */
    if (f->unknown_std)               { *type = UD_USB_OTHER;    return TRUE; }
    return FALSE;
}

static gchar *
usb_display_name (GUdevDevice *dev)
{
    const gchar *vendor = g_udev_device_get_property (dev, "ID_VENDOR_FROM_DATABASE");
    if (!vendor || !*vendor) vendor = g_udev_device_get_property (dev, "ID_VENDOR");
    const gchar *model  = g_udev_device_get_property (dev, "ID_MODEL_FROM_DATABASE");
    if (!model  || !*model)  model  = g_udev_device_get_property (dev, "ID_MODEL");

    /* el nombre que el propio aparato reporta suele ser el más claro */
    const gchar *product = g_udev_device_get_sysfs_attr (dev, "product");

    gchar *name;
    if (vendor && *vendor && model && *model)
        name = g_strdup_printf ("%s %s", vendor, model);
    else if (product && *product)
        name = g_strdup (product);
    else if (model && *model)
        name = g_strdup (model);
    else if (vendor && *vendor)
        name = g_strdup (vendor);
    else
        name = g_strdup ("Unknown USB device");

    for (gchar *c = name; *c; c++)
        if (*c == '_') *c = ' ';
    return name;
}

GSList *
ud_get_usb_devices (void)
{
    const gchar *subsystems[] = { "usb", NULL };
    GUdevClient *client = g_udev_client_new (subsystems);
    GSList      *result = NULL;
    GHashTable  *flags  = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

    mark_children (client, "input",     classify_input,  flags);
    mark_children (client, "hidraw",    classify_hidraw, flags);
    mark_children (client, "net",       classify_net,    flags);
    mark_children (client, "bluetooth", classify_bt,     flags);
    mark_children (client, "tty",       classify_tty,    flags);

    GList *usb_devs = g_udev_client_query_by_subsystem (client, "usb");
    for (GList *l = usb_devs; l; l = l->next) {
        GUdevDevice *dev        = l->data;
        const gchar *sysfs_path = g_udev_device_get_sysfs_path (dev);
        UsbIfaces    f          = { 0 };
        UdUsbType    type;

        if (g_strcmp0 (g_udev_device_get_devtype (dev), "usb_device") != 0 ||
            g_strcmp0 (g_udev_device_get_sysfs_attr (dev, "bDeviceClass"), "09") == 0 ||
            g_strcmp0 (g_udev_device_get_sysfs_attr (dev, "removable"), "fixed") == 0) {
            g_object_unref (dev);
            continue;
        }

        read_interfaces (sysfs_path, &f);
        guint    ch  = GPOINTER_TO_UINT (g_hash_table_lookup (flags, sysfs_path));
        gboolean mtp = g_udev_device_get_property_as_boolean (dev, "ID_MTP_DEVICE");

        if (classify_usb (&f, ch, mtp, &type)) {
            UdUsbDevice *ud = g_new0 (UdUsbDevice, 1);
            ud->name    = usb_display_name (dev);
            ud->type    = type;
            ud->syspath = g_strdup (sysfs_path);
            ud->devfile = g_strdup (g_udev_device_get_device_file (dev));
            result = g_slist_append (result, ud);
        }
        g_object_unref (dev);
    }

    g_list_free (usb_devs);
    g_hash_table_destroy (flags);
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
