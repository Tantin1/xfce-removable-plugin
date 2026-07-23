#ifndef UD_DBUS_H
#define UD_DBUS_H

#include <gio/gio.h>

#define UD_BUS_NAME         "org.freedesktop.UDisks2"
#define UD_OBJECT_PATH      "/org/freedesktop/UDisks2"
#define UD_BLOCK_IFACE      "org.freedesktop.UDisks2.Block"
#define UD_FILESYSTEM_IFACE "org.freedesktop.UDisks2.Filesystem"
#define UD_DRIVE_IFACE      "org.freedesktop.UDisks2.Drive"

typedef struct {
    gchar    *object_path;
    gchar    *device;
    gchar    *label;
    gchar    *drive_path;
    guint64   size;
    gboolean  mounted;
    gchar    *mount_point;
    gboolean  ejectable;
} UdDevice;

GDBusConnection *ud_dbus_connect     (void);
GSList          *ud_get_devices      (GDBusConnection *conn);
void             ud_device_list_free (GSList *list);
gchar           *ud_mount            (GDBusConnection *conn, const gchar *object_path);
gboolean         ud_unmount          (GDBusConnection *conn, const gchar *object_path);
gboolean         ud_eject            (GDBusConnection *conn, const gchar *drive_path);
gchar           *ud_get_drive_name   (GDBusConnection *conn, const gchar *drive_path);

typedef void (*UdSignalCallback) (gpointer user_data);
typedef struct UdSubscription UdSubscription;
UdSubscription  *ud_subscribe        (GDBusConnection *conn,
                                      UdSignalCallback callback,
                                      gpointer         user_data);
void             ud_unsubscribe      (GDBusConnection *conn, UdSubscription *sub);

/* --- otros dispositivos USB --- */

typedef struct {
    gchar *name;
    gchar *type;
    gchar *syspath;   /* identificador único del puerto USB ocupado */
} UdUsbDevice;

GSList          *ud_get_usb_devices      (void);
void             ud_usb_device_list_free (GSList *list);

#endif /* UD_DBUS_H */
