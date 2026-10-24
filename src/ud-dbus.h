#ifndef UD_DBUS_H
#define UD_DBUS_H

#include <gio/gio.h>

#define UD_BUS_NAME         "org.freedesktop.UDisks2"
#define UD_OBJECT_PATH      "/org/freedesktop/UDisks2"
#define UD_BLOCK_IFACE      "org.freedesktop.UDisks2.Block"
#define UD_FILESYSTEM_IFACE "org.freedesktop.UDisks2.Filesystem"
#define UD_PARTITION_IFACE  "org.freedesktop.UDisks2.Partition"
#define UD_DRIVE_IFACE      "org.freedesktop.UDisks2.Drive"

typedef struct {
    gchar    *object_path;
    gchar    *device;
    gchar    *label;
    gchar    *fs_type;        /* IdType: ext4, vfat, exfat, ntfs… (puede ser "") */
    gchar    *drive_path;
    gchar    *drive_name;     /* Vendor + Model del Drive (puede ser "") */
    guint64   size;
    gboolean  mounted;
    gchar    *mount_point;
    gboolean  is_partition;   /* tiene interfaz Partition (sdb1, mmcblk0p1…) */
    gboolean  ejectable;
    gboolean  can_power_off;
} UdDevice;

GDBusConnection *ud_dbus_connect     (void);
GSList          *ud_get_devices      (GDBusConnection *conn);
void             ud_device_list_free (GSList *list);

/* --- operaciones asíncronas (no bloquean el panel) ---
 * ok:     TRUE si la operación tuvo éxito
 * result: punto de montaje para ud_mount_async, NULL en los demás casos
 * error:  error DBus si ok == FALSE (no liberarlo; vive solo durante el callback) */
typedef void (*UdResultCallback) (gboolean      ok,
                                  const gchar  *result,
                                  const GError *error,
                                  gpointer      user_data);

void             ud_mount_async      (GDBusConnection *conn, const gchar *object_path,
                                      UdResultCallback callback, gpointer user_data);
void             ud_unmount_async    (GDBusConnection *conn, const gchar *object_path,
                                      UdResultCallback callback, gpointer user_data);
void             ud_eject_async      (GDBusConnection *conn, const gchar *drive_path,
                                      UdResultCallback callback, gpointer user_data);
void             ud_power_off_async  (GDBusConnection *conn, const gchar *drive_path,
                                      UdResultCallback callback, gpointer user_data);

typedef void (*UdSignalCallback) (gpointer user_data);
typedef struct UdSubscription UdSubscription;
UdSubscription  *ud_subscribe        (GDBusConnection *conn,
                                      UdSignalCallback callback,
                                      gpointer         user_data);
void             ud_unsubscribe      (GDBusConnection *conn, UdSubscription *sub);

/* --- otros dispositivos USB --- */

/* el valor se guarda como entero en los snapshots: agregar SOLO al final */
typedef enum {
    UD_USB_OTHER = 0,
    UD_USB_PRINTER,
    UD_USB_AUDIO,
    UD_USB_CAMERA,
    UD_USB_GAMEPAD,
    UD_USB_PHONE,     /* MTP/PTP (clase 06 o ID_MTP_DEVICE) que gvfs no tomó */
    UD_USB_TABLET,    /* tableta gráfica (ID_INPUT_TABLET) */
    UD_USB_SERIAL,    /* placa / adaptador serie: Arduino, ESP32, FTDI, CH340… */
    UD_USB_SECURITY,  /* llave de seguridad FIDO / lector de tarjetas inteligentes */
    UD_USB_MIDI,      /* teclado / controlador MIDI */
} UdUsbType;

typedef struct {
    gchar     *name;
    UdUsbType  type;
    gchar     *syspath;   /* identificador único del puerto USB ocupado */
    gchar     *devfile;   /* /dev/bus/usb/BBB/DDD (para cruzar con gvfs), puede ser NULL */
} UdUsbDevice;

GSList          *ud_get_usb_devices      (void);
void             ud_usb_device_list_free (GSList *list);

/* etiqueta SIN traducir (marcada con N_): pasarla por _() al mostrarla */
const gchar     *ud_usb_type_label       (UdUsbType type);
const gchar     *ud_usb_type_icon        (UdUsbType type);

/* --- teléfonos y cámaras vía gvfs (ud-phones.c) ---
 * Un teléfono en modo "transferencia de archivos" (MTP), una cámara (PTP) o un
 * iPhone (afc) no es un dispositivo de bloque: UDisks2 no lo ve. gvfs lo expone
 * como GVolume/GMount con URI mtp://, gphoto2:// o afc://. */

typedef enum {
    UD_PHONE_MTP = 0,
    UD_PHONE_PTP,
    UD_PHONE_AFC,
} UdPhoneKind;

typedef struct {
    gchar       *key;      /* devfile USB, o la URI si no hay: identifica el aparato */
    gchar       *name;
    UdPhoneKind  kind;
    GIcon       *icon;     /* ref; puede ser NULL */
    gchar       *uri;      /* raíz del montaje, o activation root si no está montado */
    gchar       *devfile;  /* /dev/bus/usb/BBB/DDD, puede ser NULL */
    GVolume     *volume;   /* ref; NULL si es un montaje sin volumen */
    GMount      *mount;    /* ref; NULL si no está montado */
} UdPhone;

GSList          *ud_get_phones           (void);
void             ud_phone_list_free      (GSList *list);

/* --- escaneo completo, compartido por popup, botón y notificaciones ---
 * usb ya viene sin los aparatos que aparecen en phones. */
typedef struct {
    GSList *storage;   /* UdDevice   */
    GSList *phones;    /* UdPhone    */
    GSList *usb;       /* UdUsbDevice */
} UdScan;

UdScan          *ud_scan                 (GDBusConnection *conn);
void             ud_scan_free            (UdScan *scan);
gboolean         ud_scan_is_empty        (const UdScan *scan);

#endif /* UD_DBUS_H */
