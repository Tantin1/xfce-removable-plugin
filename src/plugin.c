#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <libxfce4panel/libxfce4panel.h>
#include <gtk/gtk.h>
#include <libnotify/notify.h>
#include <canberra-gtk.h>

#include "popup.h"
#include "ud-dbus.h"
#include "rp-notify.h"

/* _() = dgettext (GETTEXT_PACKAGE, …): en Wayland todos los plugins comparten
   el proceso del panel, y textdomain() cambiaría el idioma de los demás */
#include <glib/gi18n-lib.h>

/* ---------- defaults ---------- */

#define DEFAULT_SQUARE_BUTTON        FALSE
#define DEFAULT_PLUGIN_SIZE          24
#define DEFAULT_USE_PANEL_ICON       FALSE
#define DEFAULT_ICON_SIZE            16
#define DEFAULT_POPUP_WIDTH          400
/* con Montar/Desmontar + Expulsar en una fila, menos de esto no entra en todos
   los idiomas sin ensanchar el popup (medido con ruso, el más largo) */
#define MIN_POPUP_WIDTH              320
#define DEFAULT_POPUP_HEIGHT         300
#define DEFAULT_POPUP_STYLE          POPUP_STYLE_AUTO
#define DEFAULT_SHOW_NOTIFY          TRUE
#define DEFAULT_HIDE_IF_EMPTY        TRUE
#define DEFAULT_HIDE_IF_ONLY_OTHER   FALSE
#define DEFAULT_NOTIFY_NEW_DEVICE    TRUE
#define DEFAULT_SHOW_INPUT           FALSE   /* teclados y mouses en "Other devices" */
#define DEFAULT_NOTIFY_DRIVES        TRUE
#define DEFAULT_PLAY_SOUNDS          TRUE    /* sonido al conectar/desconectar */

/* Categorías que se pueden silenciar (Propiedades → "Notify about:"), en el
   orden en que se muestran. La clave es el nombre en [notify] del .rc: un
   nombre fijo y no el número del tipo, así el archivo se entiende y sobrevive
   a cambios del enum. Los discos van aparte (notify_drives). */
static const struct {
    UdUsbType    type;
    const gchar *key;
    gboolean     def;
} NOTIFY_TYPES[] = {
    { UD_USB_PHONE,    "phones",         TRUE  },
    { UD_USB_ANDROID,  "android",        TRUE  },
    { UD_USB_GAMEPAD,  "gamepads",       TRUE  },
    { UD_USB_TABLET,   "tablets",        TRUE  },
    { UD_USB_SECURITY, "security_keys",  TRUE  },
    { UD_USB_PRINTER,  "printers",       TRUE  },
    { UD_USB_SCANNER,  "scanners",       TRUE  },
    { UD_USB_CAMERA,   "cameras",        TRUE  },
    { UD_USB_AUDIO,    "audio",          TRUE  },
    { UD_USB_MIDI,     "midi",           TRUE  },
    { UD_USB_SERIAL,   "serial",         TRUE  },
    { UD_USB_INPUT,    "keyboards_mice", FALSE },   /* se enchufan y desenchufan seguido */
};

/* ---------- struct del plugin ---------- */

typedef struct {
    XfcePanelPlugin *plugin;
    GtkWidget       *button;
    GtkWidget       *icon;
    GDBusConnection *conn;
    UdSubscription  *subscription;
    RemovablePopup  *popup;
    gboolean         popup_open;

    /* configuración */
    gboolean         square_button;
    gint             plugin_size;
    gboolean         use_panel_icon;
    gint             icon_size;
    gint             popup_width;
    gint             popup_height;
    gint             popup_style;     /* POPUP_STYLE_* (popup.h) */
    gboolean         show_notify;
    gboolean         hide_if_empty;
    gboolean         hide_if_only_other;
    gboolean         notify_new_device;
    gboolean         show_input;      /* listar teclados y mouses */
    gboolean         notify_drives;   /* notificar discos (conectar/desconectar) */
    gboolean         play_sounds;     /* sonido al conectar/desconectar (libcanberra) */
    gboolean         notify_type[UD_USB_N_TYPES];   /* por categoría (NOTIFY_TYPES) */

    /* snapshots para detectar enchufado/desenchufado */
    GHashTable      *seen_storage;   /* object_path -> label/device */
    GHashTable      *seen_usb;       /* syspath     -> name|type    */
    GHashTable      *seen_phones;    /* devfile/URI -> name         */

    /* CSS tamaño del botón */
    GtkCssProvider  *button_css;

    /* "modo primera vez": la instancia se acaba de agregar al panel (no tiene
       configuración guardada). Dura hasta que se reinicie el panel. */
    gboolean         first_run;

    /* ventana "Acerca de" abierta (una sola a la vez) */
    GtkWidget       *about_dialog;
} RemovablePlugin;

/* ---------- apply_config ---------- */

static void
apply_config (RemovablePlugin *rp)
{
    XfcePanelPlugin *plugin = rp->plugin;

    /* tamaño del botón via CSS */
    {
        gchar *css;
        /* alto de UNA fila: con el panel en 2 o más filas el plugin ocupa una
           celda (set_small), y el tamaño del panel es el de todas juntas */
        gint   ps = xfce_panel_plugin_get_size (plugin) /
                    MAX (1, (gint) xfce_panel_plugin_get_nrows (plugin));

        if (rp->square_button)
            css = g_strdup_printf (
                "button{padding:0;margin:0;min-width:%dpx;min-height:%dpx;border:none;}",
                ps, ps);
        else {
            GtkOrientation orient = xfce_panel_plugin_get_orientation (plugin);
            if (orient == GTK_ORIENTATION_HORIZONTAL)
                css = g_strdup_printf (
                    "button{padding:0;margin:0;min-width:%dpx;min-height:0;border:none;}",
                    rp->plugin_size);
            else
                css = g_strdup_printf (
                    "button{padding:0;margin:0;min-width:0;min-height:%dpx;border:none;}",
                    rp->plugin_size);
        }

        if (!rp->button_css) {
            rp->button_css = gtk_css_provider_new ();
            gtk_style_context_add_provider (
                gtk_widget_get_style_context (rp->button),
                GTK_STYLE_PROVIDER (rp->button_css),
                GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        }
        gtk_css_provider_load_from_data (rp->button_css, css, -1, NULL);
        g_free (css);
    }

    /* tamaño del ícono */
    if (rp->use_panel_icon) {
        gint icon_px = xfce_panel_plugin_get_icon_size (plugin);
        gtk_image_set_pixel_size (GTK_IMAGE (rp->icon), icon_px);
    } else {
        gtk_image_set_pixel_size (GTK_IMAGE (rp->icon), rp->icon_size);
    }
}

/* ---------- configuración persistente ---------- */

/* Un archivo por instancia del plugin (lo administra el panel:
   ~/.config/xfce4/panel/xfce-removable-plugin-<id>.rc), así dos instancias
   no se pisan. Si la instancia no tiene el suyo, se acaba de agregar al panel.
   (El .ini compartido de las versiones de desarrollo ya no se lee: ninguna
   versión publicada lo escribió, y leerlo escondía el "modo primera vez".) */

/* lee una clave booleana respetando el default si la clave no existe */
static gboolean
kf_get_bool (GKeyFile *kf, const gchar *group, const gchar *key, gboolean def)
{
    if (!g_key_file_has_key (kf, group, key, NULL))
        return def;
    return g_key_file_get_boolean (kf, group, key, NULL);
}

/* lee una clave entera respetando el default si no existe o es 0 */
static gint
kf_get_int (GKeyFile *kf, const gchar *group, const gchar *key, gint def)
{
    gint v;
    if (!g_key_file_has_key (kf, group, key, NULL))
        return def;
    v = g_key_file_get_integer (kf, group, key, NULL);
    return (v != 0) ? v : def;
}

static void
load_config (RemovablePlugin *rp)
{
    GKeyFile *kf   = g_key_file_new ();
    gchar    *path = xfce_panel_plugin_lookup_rc_file (rp->plugin);
    GError   *err  = NULL;

    /* sin archivo propio: instancia recién agregada */
    rp->first_run = (path == NULL);

    rp->square_button       = DEFAULT_SQUARE_BUTTON;
    rp->plugin_size         = DEFAULT_PLUGIN_SIZE;
    rp->use_panel_icon      = DEFAULT_USE_PANEL_ICON;
    rp->icon_size           = DEFAULT_ICON_SIZE;
    rp->popup_width         = DEFAULT_POPUP_WIDTH;
    rp->popup_height        = DEFAULT_POPUP_HEIGHT;
    rp->popup_style         = DEFAULT_POPUP_STYLE;
    rp->show_notify         = DEFAULT_SHOW_NOTIFY;
    rp->hide_if_empty       = DEFAULT_HIDE_IF_EMPTY;
    rp->hide_if_only_other  = DEFAULT_HIDE_IF_ONLY_OTHER;
    rp->notify_new_device   = DEFAULT_NOTIFY_NEW_DEVICE;
    rp->show_input          = DEFAULT_SHOW_INPUT;
    rp->notify_drives       = DEFAULT_NOTIFY_DRIVES;
    rp->play_sounds         = DEFAULT_PLAY_SOUNDS;
    /* sin categoría (UD_USB_OTHER) nunca se notifica: queda en FALSE */
    for (gsize i = 0; i < G_N_ELEMENTS (NOTIFY_TYPES); i++)
        rp->notify_type[NOTIFY_TYPES[i].type] = NOTIFY_TYPES[i].def;

    if (!path || !g_key_file_load_from_file (kf, path, G_KEY_FILE_NONE, &err)) {
        g_clear_error (&err);
        goto out;
    }

    rp->square_button      = kf_get_bool (kf, "appearance", "square_button",      DEFAULT_SQUARE_BUTTON);
    rp->plugin_size        = kf_get_int  (kf, "appearance", "plugin_size",        DEFAULT_PLUGIN_SIZE);
    rp->use_panel_icon     = kf_get_bool (kf, "appearance", "use_panel_icon",     DEFAULT_USE_PANEL_ICON);
    rp->icon_size          = kf_get_int  (kf, "appearance", "icon_size",          DEFAULT_ICON_SIZE);
    rp->popup_width        = MAX (MIN_POPUP_WIDTH,
                                  kf_get_int (kf, "appearance", "popup_width", DEFAULT_POPUP_WIDTH));
    rp->popup_height       = kf_get_int  (kf, "appearance", "popup_height",       DEFAULT_POPUP_HEIGHT);
    /* kf_get_int toma 0 como "sin valor", y 0 es justo Automático: leer directo y validar */
    if (g_key_file_has_key (kf, "appearance", "popup_style", NULL)) {
        gint st = g_key_file_get_integer (kf, "appearance", "popup_style", NULL);
        rp->popup_style = (st >= POPUP_STYLE_AUTO && st <= POPUP_STYLE_ATTACHED)
                          ? st : DEFAULT_POPUP_STYLE;
    }
    rp->show_notify        = kf_get_bool (kf, "behavior",   "show_notify",        DEFAULT_SHOW_NOTIFY);
    rp->hide_if_empty      = kf_get_bool (kf, "behavior",   "hide_if_empty",      DEFAULT_HIDE_IF_EMPTY);
    rp->hide_if_only_other = kf_get_bool (kf, "behavior",   "hide_if_only_other", DEFAULT_HIDE_IF_ONLY_OTHER);
    rp->notify_new_device  = kf_get_bool (kf, "behavior",   "notify_new_device",  DEFAULT_NOTIFY_NEW_DEVICE);
    rp->show_input         = kf_get_bool (kf, "behavior",   "show_input",         DEFAULT_SHOW_INPUT);
    rp->play_sounds        = kf_get_bool (kf, "behavior",   "play_sounds",        DEFAULT_PLAY_SOUNDS);
    rp->notify_drives      = kf_get_bool (kf, "notify",     "drives",             DEFAULT_NOTIFY_DRIVES);
    for (gsize i = 0; i < G_N_ELEMENTS (NOTIFY_TYPES); i++)
        rp->notify_type[NOTIFY_TYPES[i].type] =
            kf_get_bool (kf, "notify", NOTIFY_TYPES[i].key, NOTIFY_TYPES[i].def);

out:
    g_key_file_free (kf);
    g_free (path);
}

static void
save_config (RemovablePlugin *rp)
{
    gchar    *path = xfce_panel_plugin_save_location (rp->plugin, TRUE);
    GError   *err  = NULL;

    if (!path) {
        g_warning ("xfce-removable-plugin: el panel no dio ruta para guardar la config");
        return;
    }

    GKeyFile *kf = g_key_file_new ();

    g_key_file_set_boolean (kf, "appearance", "square_button",      rp->square_button);
    g_key_file_set_integer  (kf, "appearance", "plugin_size",        rp->plugin_size);
    g_key_file_set_boolean (kf, "appearance", "use_panel_icon",     rp->use_panel_icon);
    g_key_file_set_integer  (kf, "appearance", "icon_size",          rp->icon_size);
    g_key_file_set_integer  (kf, "appearance", "popup_width",        rp->popup_width);
    g_key_file_set_integer  (kf, "appearance", "popup_height",       rp->popup_height);
    g_key_file_set_integer  (kf, "appearance", "popup_style",        rp->popup_style);
    g_key_file_set_boolean  (kf, "behavior",   "show_notify",        rp->show_notify);
    g_key_file_set_boolean  (kf, "behavior",   "hide_if_empty",      rp->hide_if_empty);
    g_key_file_set_boolean  (kf, "behavior",   "hide_if_only_other", rp->hide_if_only_other);
    g_key_file_set_boolean  (kf, "behavior",   "notify_new_device",  rp->notify_new_device);
    g_key_file_set_boolean  (kf, "behavior",   "show_input",         rp->show_input);
    g_key_file_set_boolean  (kf, "behavior",   "play_sounds",        rp->play_sounds);
    g_key_file_set_boolean  (kf, "notify",     "drives",             rp->notify_drives);
    for (gsize i = 0; i < G_N_ELEMENTS (NOTIFY_TYPES); i++)
        g_key_file_set_boolean (kf, "notify", NOTIFY_TYPES[i].key,
                                rp->notify_type[NOTIFY_TYPES[i].type]);

    if (!g_key_file_save_to_file (kf, path, &err)) {
        g_warning ("xfce-removable-plugin: no se pudo guardar config: %s", err->message);
        g_error_free (err);
    }

    g_key_file_free (kf);
    g_free (path);
}

/* ---------- escaneo ---------- */

/* ud_scan + lo que el usuario eligió ver. Usarla siempre en vez de ud_scan
   (popup.c hace lo mismo con el dato "show-input" de su ventana). */
static UdScan *
plugin_scan (RemovablePlugin *rp)
{
    UdScan *scan = ud_scan (rp->conn);
    if (!rp->show_input)
        ud_scan_drop_usb_type (scan, UD_USB_INPUT);
    return scan;
}

/* ---------- visibilidad del botón ---------- */

static void
apply_button_visibility (RemovablePlugin *rp, const UdScan *scan)
{
    /* los teléfonos cuentan como dispositivos "principales", no como "otros" */
    gboolean show = rp->first_run ||        /* recién agregado: que se vea y se pueda ubicar */
                    !rp->hide_if_empty ||
                    scan->storage != NULL ||
                    scan->phones  != NULL ||
                    (!rp->hide_if_only_other && scan->usb != NULL);

    if (show) {
        gtk_widget_show (rp->button);
        return;
    }
    gtk_widget_hide (rp->button);
    if (rp->popup_open)
        popup_hide (rp->popup);
}

/* para cuando no hay listas a mano (cambios en Propiedades) */
static void
plugin_update_button_visibility (RemovablePlugin *rp)
{
    UdScan *scan = plugin_scan (rp);
    apply_button_visibility (rp, scan);
    ud_scan_free (scan);
}

/* ---------- toggle del popup ---------- */

static void
plugin_popup_toggle (GtkWidget *button, RemovablePlugin *rp)
{
    (void) button;

    if (rp->popup_open) {
        popup_hide (rp->popup);
        return;
    }

    rp->popup_open = TRUE;
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (rp->button), TRUE);
    popup_show (rp->popup, rp->plugin, rp->button, rp->conn,
                rp->popup_width, rp->popup_height);
}

/* callback para cerrar el popup desde popup.c */
static void
on_popup_closed (RemovablePlugin *rp)
{
    rp->popup_open = FALSE;
    g_signal_handlers_block_by_func (rp->button, plugin_popup_toggle, rp);
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (rp->button), FALSE);
    g_signal_handlers_unblock_by_func (rp->button, plugin_popup_toggle, rp);
}

/* ---------- notificaciones de enchufado/desenchufado ---------- */

static gchar *
format_size_short (guint64 bytes)
{
    if (bytes >= (guint64) 1e9) return g_strdup_printf ("%.1f GB", bytes / 1e9);
    if (bytes >= (guint64) 1e6) return g_strdup_printf ("%.0f MB", bytes / 1e6);
    return g_strdup_printf ("%.0f KB", bytes / 1e3);
}

/* Sonidos de conectar/desconectar con libcanberra: nombres del tema de sonidos
   de freedesktop ("device-added" / "device-removed"), o los del tema que eligió
   el usuario. libcanberra-gtk sigue sola el interruptor de Xfce "sonidos de
   eventos" (gtk-enable-event-sounds): si está apagado no suena nada, sin que el
   plugin tenga que mirarlo. */
static void
play_device_sound (RemovablePlugin *rp, gboolean added)
{
    ca_context *ctx = ca_gtk_context_get_for_screen (gtk_widget_get_screen (rp->button));
    if (!ctx)
        return;
    ca_context_play (ctx, 0,
                     CA_PROP_EVENT_ID, added ? "device-added" : "device-removed",
                     CA_PROP_EVENT_DESCRIPTION, added ? "Device connected" : "Device disconnected",
                     NULL);
}

/* Un cambio de conexión: notificación (si están activadas) y marca para el
   sonido, que se reproduce una sola vez por refresco aunque cambien varios.
   key = el aparato: su burbuja reemplaza a la anterior del mismo aparato.
   Una desconexión esperada (se acaba de expulsar desde el popup) no se
   notifica: taparía el "Ya puede desenchufarlo". Con el popup abierto
   tampoco: la lista ya se actualiza sola y la burbuja lo taparía.
   El sonido suena igual en los dos casos. */
static void
announce (RemovablePlugin *rp, gboolean *changed, const gchar *key, gboolean removal,
          const gchar *summary, const gchar *body, const gchar *icon)
{
    *changed = TRUE;
    if (removal && rp_notify_removal_expected (key))   /* consume la marca siempre */
        return;
    if (rp->notify_new_device && !rp->popup_open)
        rp_notify (key, summary, body, icon);
}

/* ¿se notifica este tipo de "Other devices"? (los snapshots guardan el tipo
   como entero: validar el rango por si viene de una versión con más tipos) */
static gboolean
notify_usb_type (RemovablePlugin *rp, UdUsbType type)
{
    return type > UD_USB_OTHER && type < UD_USB_N_TYPES && rp->notify_type[type];
}

/* construye las tablas con el estado actual y, si silent==FALSE, notifica
   las diferencias contra las tablas anteriores en rp->seen_storage / rp->seen_usb.
   No libera devs/usb: son del llamador. */
static void
update_snapshots (RemovablePlugin *rp, const UdScan *scan, gboolean silent)
{
    GSList     *devs        = scan->storage;
    GHashTable *new_storage = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                     g_free, g_free);
    GHashTable *new_usb     = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                     g_free, g_free);
    GHashTable *new_phones  = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                     g_free, g_free);

    /* almacenamiento — agrupado por drive_path para no notificar
       una vez por cada partición. Valor: "nombre|tamaño" */
    for (GSList *l = devs; l; l = l->next) {
        UdDevice *d = l->data;
        if (!d->drive_path) continue;
        if (g_hash_table_contains (new_storage, d->drive_path)) {
            /* otra partición del mismo disco: sumar tamaño */
            gchar **prev = g_strsplit ((const gchar *) g_hash_table_lookup (
                                           new_storage, d->drive_path), "|", 2);
            guint64 prev_size = prev[1] ? g_ascii_strtoull (prev[1], NULL, 10) : 0;
            gchar *value = g_strdup_printf ("%s|%" G_GUINT64_FORMAT,
                                            prev[0] ? prev[0] : "",
                                            prev_size + d->size);
            g_hash_table_replace (new_storage, g_strdup (d->drive_path), value);
            g_strfreev (prev);
            continue;
        }
        /* primera partición vista de este disco: nombre del Drive */
        const gchar *display = (d->drive_name && *d->drive_name) ? d->drive_name :
                               ((d->label && *d->label) ? d->label : d->device);
        gchar *value = g_strdup_printf ("%s|%" G_GUINT64_FORMAT,
                                        display ? display : "", d->size);
        g_hash_table_replace (new_storage, g_strdup (d->drive_path), value);
    }

    /* teléfonos. Clave = devfile USB (o URI), igual para los que vienen de gvfs
       y para los UD_USB_PHONE que gvfs todavía no tomó: así el paso de uno a
       otro (gvfs tarda un poco más que udev) no notifica desconexión+conexión */
    for (GSList *l = scan->phones; l; l = l->next) {
        UdPhone *ph = l->data;
        if (!ph->key) continue;
        g_hash_table_replace (new_phones, g_strdup (ph->key), g_strdup (ph->name ? ph->name : ""));
    }

    /* otros USB. Valor: "nombre|tipo" (tipo = UdUsbType como entero) */
    for (GSList *l = scan->usb; l; l = l->next) {
        UdUsbDevice *u = l->data;
        if (u->type == UD_USB_PHONE) {
            const gchar *k = u->devfile ? u->devfile : u->syspath;
            if (k && !g_hash_table_contains (new_phones, k))
                g_hash_table_replace (new_phones, g_strdup (k), g_strdup (u->name ? u->name : ""));
            continue;
        }
        if (!u->syspath) continue;
        gchar *value = g_strdup_printf ("%s|%d", u->name ? u->name : "", (gint) u->type);
        g_hash_table_replace (new_usb, g_strdup (u->syspath), value);
    }

    /* los sonidos siguen los mismos filtros por categoría que las notificaciones */
    if (!silent && (rp->notify_new_device || rp->play_sounds)) {
        /* enchufado: en new pero no en seen */
        GHashTableIter it;
        gpointer key, value;
        gboolean added = FALSE, removed = FALSE;

        /* las tablas se actualizan igual al final aunque una categoría esté
           silenciada: así reactivarla no notifica lo que ya estaba enchufado */
        g_hash_table_iter_init (&it, new_storage);
        while (rp->notify_drives && g_hash_table_iter_next (&it, &key, &value)) {
            if (!rp->seen_storage || !g_hash_table_contains (rp->seen_storage, key)) {
                gchar **parts = g_strsplit ((const gchar *) value, "|", 2);
                guint64 size  = parts[1] ? g_ascii_strtoull (parts[1], NULL, 10) : 0;
                gchar  *sz    = format_size_short (size);
                gchar  *body  = g_strdup_printf ("%s (%s)",
                                                 (parts[0] && *parts[0]) ? parts[0] : _("Unknown"),
                                                 sz);
                announce (rp, &added, key, FALSE, _("Removable device connected"), body, "drive-removable-media");
                g_free (body); g_free (sz); g_strfreev (parts);
            }
        }
        g_hash_table_iter_init (&it, new_usb);
        while (g_hash_table_iter_next (&it, &key, &value)) {
            if (!rp->seen_usb || !g_hash_table_contains (rp->seen_usb, key)) {
                gchar   **parts = g_strsplit ((const gchar *) value, "|", 2);
                UdUsbType type  = parts[1] ? (UdUsbType) g_ascii_strtoll (parts[1], NULL, 10)
                                           : UD_USB_OTHER;
                /* sin categoría (o silenciada): se lista pero no se notifica */
                if (!notify_usb_type (rp, type)) { g_strfreev (parts); continue; }
                gchar    *body  = g_strdup_printf ("%s — %s",
                                                   (parts[0] && *parts[0]) ? parts[0] : _("Unknown"),
                                                   _(ud_usb_type_label (type)));
                announce (rp, &added, key, FALSE, _("USB device connected"), body, ud_usb_type_icon (type));
                g_free (body); g_strfreev (parts);
            }
        }

        g_hash_table_iter_init (&it, new_phones);
        while (rp->notify_type[UD_USB_PHONE] && g_hash_table_iter_next (&it, &key, &value)) {
            if (!rp->seen_phones || !g_hash_table_contains (rp->seen_phones, key))
                announce (rp, &added, key, FALSE, _("Phone or camera connected"),
                          *(const gchar *) value ? (const gchar *) value : _("Unknown"),
                          "phone");
        }

        /* desenchufado: en seen pero no en new */
        if (rp->seen_phones && rp->notify_type[UD_USB_PHONE]) {
            g_hash_table_iter_init (&it, rp->seen_phones);
            while (g_hash_table_iter_next (&it, &key, &value)) {
                if (!g_hash_table_contains (new_phones, key))
                    announce (rp, &removed, key, TRUE, _("Phone or camera disconnected"),
                              *(const gchar *) value ? (const gchar *) value : _("Unknown"),
                              "phone");
            }
        }
        if (rp->seen_storage && rp->notify_drives) {
            g_hash_table_iter_init (&it, rp->seen_storage);
            while (g_hash_table_iter_next (&it, &key, &value)) {
                if (!g_hash_table_contains (new_storage, key)) {
                    gchar **parts = g_strsplit ((const gchar *) value, "|", 2);
                    announce (rp, &removed, key, TRUE, _("Removable device disconnected"),
                              (parts[0] && *parts[0]) ? parts[0] : _("Unknown"),
                              "drive-removable-media");
                    g_strfreev (parts);
                }
            }
        }
        if (rp->seen_usb) {
            g_hash_table_iter_init (&it, rp->seen_usb);
            while (g_hash_table_iter_next (&it, &key, &value)) {
                if (!g_hash_table_contains (new_usb, key)) {
                    gchar   **parts = g_strsplit ((const gchar *) value, "|", 2);
                    UdUsbType type  = parts[1] ? (UdUsbType) g_ascii_strtoll (parts[1], NULL, 10)
                                               : UD_USB_OTHER;
                    if (!notify_usb_type (rp, type)) { g_strfreev (parts); continue; }
                    announce (rp, &removed, key, TRUE, _("USB device disconnected"),
                              (parts[0] && *parts[0]) ? parts[0] : _("Unknown"),
                              ud_usb_type_icon (type));
                    g_strfreev (parts);
                }
            }
        }

        /* un solo sonido por refresco; si a la vez se conectó uno y se quitó
           otro (raro: el debounce los junta), gana "conectado" */
        if (rp->play_sounds && (added || removed))
            play_device_sound (rp, added);
    }

    if (rp->seen_storage) g_hash_table_destroy (rp->seen_storage);
    if (rp->seen_usb)     g_hash_table_destroy (rp->seen_usb);
    if (rp->seen_phones)  g_hash_table_destroy (rp->seen_phones);
    rp->seen_storage = new_storage;
    rp->seen_usb     = new_usb;
    rp->seen_phones  = new_phones;
}

/* ---------- refresco por señal DBus ---------- */

static void
plugin_on_device_change (RemovablePlugin *rp)
{
    /* un solo escaneo por refresco, compartido por snapshots, botón y popup */
    UdScan *scan = plugin_scan (rp);

    update_snapshots (rp, scan, FALSE);
    apply_button_visibility (rp, scan);
    if (rp->popup_open)
        popup_refresh (rp->popup, rp->conn, scan);

    ud_scan_free (scan);
}

/* ---------- ventana de Propiedades ---------- */

typedef struct {
    RemovablePlugin *rp;
    GtkWidget       *square_check;
    GtkWidget       *plugin_spin;
    GtkWidget       *panel_icon_check;
    GtkWidget       *icon_spin;
    GtkWidget       *popup_width_spin;
    GtkWidget       *popup_height_spin;
    GtkWidget       *popup_style_combo;
    GtkWidget       *notify_check;
    GtkWidget       *hide_check;
    GtkWidget       *hide_other_check;
    GtkWidget       *notify_new_check;
    GtkWidget       *sounds_check;
    GtkWidget       *show_input_check;
    GtkWidget       *notify_about_label;
    GtkWidget       *notify_drives_check;
    GtkWidget       *notify_type_check[UD_USB_N_TYPES];   /* NULL si no hay casilla */
} PropsDialog;

/* las casillas por categoría valen para las notificaciones de conexión y para
   los sonidos: activas si hay alguna de las dos; la de teclados y mouses,
   además, solo si se muestran */
static void
props_update_notify_sensitivity (PropsDialog *pd)
{
    gboolean on = pd->rp->notify_new_device || pd->rp->play_sounds;
    gtk_widget_set_sensitive (pd->notify_about_label,  on);
    gtk_widget_set_sensitive (pd->notify_drives_check, on);
    for (gint t = 0; t < UD_USB_N_TYPES; t++)
        if (pd->notify_type_check[t])
            gtk_widget_set_sensitive (pd->notify_type_check[t],
                                      on && (t != UD_USB_INPUT || pd->rp->show_input));
}

static void
on_square_toggled (GtkToggleButton *btn, PropsDialog *pd)
{
    pd->rp->square_button = gtk_toggle_button_get_active (btn);
    gtk_widget_set_sensitive (pd->plugin_spin, !pd->rp->square_button);
    save_config (pd->rp);
    apply_config (pd->rp);
}

static void
on_plugin_size_changed (GtkSpinButton *spin, PropsDialog *pd)
{
    pd->rp->plugin_size = gtk_spin_button_get_value_as_int (spin);
    save_config (pd->rp);
    apply_config (pd->rp);
}

static void
on_panel_icon_toggled (GtkToggleButton *btn, PropsDialog *pd)
{
    pd->rp->use_panel_icon = gtk_toggle_button_get_active (btn);
    gtk_widget_set_sensitive (pd->icon_spin, !pd->rp->use_panel_icon);
    save_config (pd->rp);
    apply_config (pd->rp);
}

static void
on_icon_size_changed (GtkSpinButton *spin, PropsDialog *pd)
{
    pd->rp->icon_size = gtk_spin_button_get_value_as_int (spin);
    save_config (pd->rp);
    apply_config (pd->rp);
}

static void
on_popup_width_changed (GtkSpinButton *spin, PropsDialog *pd)
{
    pd->rp->popup_width = gtk_spin_button_get_value_as_int (spin);
    save_config (pd->rp);
}

static void
on_popup_style_changed (GtkComboBox *combo, PropsDialog *pd)
{
    pd->rp->popup_style = gtk_combo_box_get_active (combo);
    save_config (pd->rp);
    popup_set_style (pd->rp->popup, pd->rp->popup_style);   /* vale desde la próxima apertura */
}

static void
on_popup_height_changed (GtkSpinButton *spin, PropsDialog *pd)
{
    pd->rp->popup_height = gtk_spin_button_get_value_as_int (spin);
    save_config (pd->rp);
}

static void
on_notify_toggled (GtkToggleButton *btn, PropsDialog *pd)
{
    pd->rp->show_notify = gtk_toggle_button_get_active (btn);
    save_config (pd->rp);
    popup_set_notify (pd->rp->popup, pd->rp->show_notify);
}

static void
on_hide_toggled (GtkToggleButton *btn, PropsDialog *pd)
{
    pd->rp->hide_if_empty = gtk_toggle_button_get_active (btn);
    gtk_widget_set_sensitive (pd->hide_other_check, pd->rp->hide_if_empty);
    save_config (pd->rp);
    popup_set_first_run_hint (pd->rp->popup, pd->rp->first_run && pd->rp->hide_if_empty);
    plugin_update_button_visibility (pd->rp);
}

static void
on_hide_other_toggled (GtkToggleButton *btn, PropsDialog *pd)
{
    pd->rp->hide_if_only_other = gtk_toggle_button_get_active (btn);
    save_config (pd->rp);
    plugin_update_button_visibility (pd->rp);
}

static void
on_notify_new_toggled (GtkToggleButton *btn, PropsDialog *pd)
{
    pd->rp->notify_new_device = gtk_toggle_button_get_active (btn);
    save_config (pd->rp);
    props_update_notify_sensitivity (pd);
}

static void
on_sounds_toggled (GtkToggleButton *btn, PropsDialog *pd)
{
    pd->rp->play_sounds = gtk_toggle_button_get_active (btn);
    save_config (pd->rp);
    props_update_notify_sensitivity (pd);
}

static void
on_show_input_toggled (GtkToggleButton *btn, PropsDialog *pd)
{
    RemovablePlugin *rp = pd->rp;

    rp->show_input = gtk_toggle_button_get_active (btn);
    save_config (rp);
    g_object_set_data (G_OBJECT (rp->popup->window), "show-input",
                       GINT_TO_POINTER (rp->show_input));
    props_update_notify_sensitivity (pd);

    /* tomar la foto nueva en silencio: si no, el próximo refresco notificaría
       como "conectados" (o "desconectados") los teclados que ya estaban */
    UdScan *scan = plugin_scan (rp);
    update_snapshots (rp, scan, TRUE);
    apply_button_visibility (rp, scan);
    ud_scan_free (scan);
}

static void
on_notify_drives_toggled (GtkToggleButton *btn, PropsDialog *pd)
{
    pd->rp->notify_drives = gtk_toggle_button_get_active (btn);
    save_config (pd->rp);
}

static void
on_notify_type_toggled (GtkToggleButton *btn, PropsDialog *pd)
{
    gint t = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (btn), "notify-type"));
    pd->rp->notify_type[t] = gtk_toggle_button_get_active (btn);
    save_config (pd->rp);
}

static void
on_props_dialog_destroy (GtkWidget *win, PropsDialog *pd)
{
    (void) win;
    g_free (pd);
}

static void
on_configure_plugin (XfcePanelPlugin *plugin, RemovablePlugin *rp)
{
    /* con el popup abierto él tiene el teclado y el diálogo no lo recibiría */
    if (rp->popup_open)
        popup_hide (rp->popup);

    PropsDialog *pd = g_new0 (PropsDialog, 1);
    pd->rp = rp;

    /* SIN padre a propósito (NULL): en Wayland el panel es una capa
       (layer-shell) y con él como padre GTK ubica mal las ventanitas del
       diálogo; la lista desplegable de "Estilo" saldría en una esquina de la
       pantalla. No "arreglarlo" poniendo el panel como padre. */
    GtkWidget *dialog = gtk_dialog_new_with_buttons (
        _("Properties — Removable Devices"),
        NULL,
        GTK_DIALOG_DESTROY_WITH_PARENT,
        _("Close"), GTK_RESPONSE_CLOSE,
        NULL);
    gtk_window_set_resizable (GTK_WINDOW (dialog), FALSE);

    GtkWidget *content = gtk_dialog_get_content_area (GTK_DIALOG (dialog));
    gtk_container_set_border_width (GTK_CONTAINER (content), 12);

    /* ---- frame Apariencia ---- */
    GtkWidget *frame_ap = gtk_frame_new (_("Appearance"));
    GtkWidget *grid_ap  = gtk_grid_new ();
    gtk_grid_set_row_spacing    (GTK_GRID (grid_ap), 6);
    gtk_grid_set_column_spacing (GTK_GRID (grid_ap), 12);
    gtk_container_set_border_width (GTK_CONTAINER (grid_ap), 8);
    gtk_container_add (GTK_CONTAINER (frame_ap), grid_ap);

    gint row = 0;

    /* botón cuadrado */
    pd->square_check = gtk_check_button_new_with_label (_("Square button"));
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (pd->square_check), rp->square_button);
    gtk_grid_attach (GTK_GRID (grid_ap), pd->square_check, 0, row, 2, 1);
    row++;

    /* tamaño del plugin */
    GtkWidget *plugin_size_label = gtk_label_new (_("Plugin size:"));
    gtk_label_set_xalign (GTK_LABEL (plugin_size_label), 0.0);
    gtk_widget_set_margin_start (plugin_size_label, 12);
    pd->plugin_spin = gtk_spin_button_new_with_range (16, 200, 1);
    gtk_spin_button_set_value (GTK_SPIN_BUTTON (pd->plugin_spin), rp->plugin_size);
    gtk_widget_set_sensitive (pd->plugin_spin, !rp->square_button);
    gtk_grid_attach (GTK_GRID (grid_ap), plugin_size_label, 0, row, 1, 1);
    gtk_grid_attach (GTK_GRID (grid_ap), pd->plugin_spin,   1, row, 1, 1);
    row++;

    /* ícono del panel */
    pd->panel_icon_check = gtk_check_button_new_with_label (_("Use panel icon size"));
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (pd->panel_icon_check), rp->use_panel_icon);
    gtk_grid_attach (GTK_GRID (grid_ap), pd->panel_icon_check, 0, row, 2, 1);
    row++;

    /* tamaño del ícono */
    GtkWidget *icon_size_label = gtk_label_new (_("Icon size:"));
    gtk_label_set_xalign (GTK_LABEL (icon_size_label), 0.0);
    gtk_widget_set_margin_start (icon_size_label, 12);
    pd->icon_spin = gtk_spin_button_new_with_range (8, 128, 1);
    gtk_spin_button_set_value (GTK_SPIN_BUTTON (pd->icon_spin), rp->icon_size);
    gtk_widget_set_sensitive (pd->icon_spin, !rp->use_panel_icon);
    gtk_grid_attach (GTK_GRID (grid_ap), icon_size_label, 0, row, 1, 1);
    gtk_grid_attach (GTK_GRID (grid_ap), pd->icon_spin,   1, row, 1, 1);
    row++;

    /* ancho del popup */
    GtkWidget *popup_width_label = gtk_label_new (_("Popup width:"));
    gtk_label_set_xalign (GTK_LABEL (popup_width_label), 0.0);
    pd->popup_width_spin = gtk_spin_button_new_with_range (MIN_POPUP_WIDTH, 800, 10);
    gtk_spin_button_set_value (GTK_SPIN_BUTTON (pd->popup_width_spin), rp->popup_width);
    gtk_grid_attach (GTK_GRID (grid_ap), popup_width_label,   0, row, 1, 1);
    gtk_grid_attach (GTK_GRID (grid_ap), pd->popup_width_spin, 1, row, 1, 1);
    row++;

    /* alto del popup */
    GtkWidget *popup_height_label = gtk_label_new (_("Popup height:"));
    gtk_label_set_xalign (GTK_LABEL (popup_height_label), 0.0);
    pd->popup_height_spin = gtk_spin_button_new_with_range (100, 800, 10);
    gtk_spin_button_set_value (GTK_SPIN_BUTTON (pd->popup_height_spin), rp->popup_height);
    gtk_grid_attach (GTK_GRID (grid_ap), popup_height_label,   0, row, 1, 1);
    gtk_grid_attach (GTK_GRID (grid_ap), pd->popup_height_spin, 1, row, 1, 1);
    row++;

    /* estilo de la ventana: el orden de los textos es el de POPUP_STYLE_* */
    GtkWidget *popup_style_label = gtk_label_new (_("Popup style:"));
    gtk_label_set_xalign (GTK_LABEL (popup_style_label), 0.0);
    pd->popup_style_combo = gtk_combo_box_text_new ();
    gtk_combo_box_text_append_text (GTK_COMBO_BOX_TEXT (pd->popup_style_combo), _("Automatic"));
    gtk_combo_box_text_append_text (GTK_COMBO_BOX_TEXT (pd->popup_style_combo), _("Floating"));
    gtk_combo_box_text_append_text (GTK_COMBO_BOX_TEXT (pd->popup_style_combo), _("Attached to the panel"));
    gtk_combo_box_set_active (GTK_COMBO_BOX (pd->popup_style_combo), rp->popup_style);
    gtk_widget_set_tooltip_text (pd->popup_style_combo,
        _("Automatic: floating with rounded corners when the theme and the "
          "compositor allow it, otherwise attached to the panel."));
    gtk_grid_attach (GTK_GRID (grid_ap), popup_style_label,     0, row, 1, 1);
    gtk_grid_attach (GTK_GRID (grid_ap), pd->popup_style_combo, 1, row, 1, 1);

    gtk_box_pack_start (GTK_BOX (content), frame_ap, FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (content),
                        gtk_separator_new (GTK_ORIENTATION_HORIZONTAL),
                        FALSE, FALSE, 6);

    /* ---- frame Comportamiento ---- */
    GtkWidget *frame_bh = gtk_frame_new (_("Behavior"));
    GtkWidget *grid_bh  = gtk_grid_new ();
    gtk_grid_set_row_spacing    (GTK_GRID (grid_bh), 6);
    gtk_grid_set_column_spacing (GTK_GRID (grid_bh), 12);
    gtk_container_set_border_width (GTK_CONTAINER (grid_bh), 8);
    gtk_container_add (GTK_CONTAINER (frame_bh), grid_bh);

    row = 0;

    pd->notify_check = gtk_check_button_new_with_label (_("Show notifications"));
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (pd->notify_check), rp->show_notify);
    gtk_grid_attach (GTK_GRID (grid_bh), pd->notify_check, 0, row, 2, 1);
    row++;

    pd->hide_check = gtk_check_button_new_with_label (_("Hide button when no removable devices"));
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (pd->hide_check), rp->hide_if_empty);
    gtk_grid_attach (GTK_GRID (grid_bh), pd->hide_check, 0, row, 2, 1);
    row++;

    /* checkbox hijo — indentado, sensible solo si hide_check está activo */
    pd->hide_other_check = gtk_check_button_new_with_label (
        _("Also hide when only other devices are present"));
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (pd->hide_other_check), rp->hide_if_only_other);
    gtk_widget_set_sensitive (pd->hide_other_check, rp->hide_if_empty);
    gtk_widget_set_margin_start (pd->hide_other_check, 20);
    gtk_grid_attach (GTK_GRID (grid_bh), pd->hide_other_check, 0, row, 2, 1);
    row++;

    pd->show_input_check = gtk_check_button_new_with_label (_("Show keyboards and mice"));
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (pd->show_input_check), rp->show_input);
    gtk_grid_attach (GTK_GRID (grid_bh), pd->show_input_check, 0, row, 2, 1);
    row++;

    pd->notify_new_check = gtk_check_button_new_with_label (
        _("Notify when devices are connected or disconnected"));
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (pd->notify_new_check), rp->notify_new_device);
    gtk_grid_attach (GTK_GRID (grid_bh), pd->notify_new_check, 0, row, 2, 1);
    row++;

    pd->sounds_check = gtk_check_button_new_with_label (
        _("Play a sound when devices are connected or disconnected"));
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (pd->sounds_check), rp->play_sounds);
    gtk_widget_set_tooltip_text (pd->sounds_check,
        _("Uses the desktop sound theme. Nothing plays if event sounds are "
          "turned off in the Appearance settings."));
    gtk_grid_attach (GTK_GRID (grid_bh), pd->sounds_check, 0, row, 2, 1);
    row++;

    /* por categoría, en dos columnas (reusa los nombres de tipo ya traducidos) */
    pd->notify_about_label = gtk_label_new (_("Notify about:"));
    gtk_label_set_xalign (GTK_LABEL (pd->notify_about_label), 0.0);
    gtk_widget_set_margin_start (pd->notify_about_label, 20);
    gtk_grid_attach (GTK_GRID (grid_bh), pd->notify_about_label, 0, row, 2, 1);
    row++;

    GtkWidget *grid_types = gtk_grid_new ();
    gtk_grid_set_row_spacing    (GTK_GRID (grid_types), 2);
    gtk_grid_set_column_spacing (GTK_GRID (grid_types), 12);
    gtk_widget_set_margin_start (grid_types, 32);
    gtk_grid_attach (GTK_GRID (grid_bh), grid_types, 0, row, 2, 1);

    pd->notify_drives_check = gtk_check_button_new_with_label (_("Removable disk"));
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (pd->notify_drives_check), rp->notify_drives);
    gtk_grid_attach (GTK_GRID (grid_types), pd->notify_drives_check, 0, 0, 1, 1);
    g_signal_connect (pd->notify_drives_check, "toggled",
                      G_CALLBACK (on_notify_drives_toggled), pd);

    for (gsize i = 0; i < G_N_ELEMENTS (NOTIFY_TYPES); i++) {
        UdUsbType  t   = NOTIFY_TYPES[i].type;
        gint       pos = (gint) i + 1;            /* 0 es "Removable disk" */
        GtkWidget *chk = gtk_check_button_new_with_label (_(ud_usb_type_label (t)));
        gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (chk), rp->notify_type[t]);
        g_object_set_data (G_OBJECT (chk), "notify-type", GINT_TO_POINTER (t));
        gtk_grid_attach (GTK_GRID (grid_types), chk, pos % 2, pos / 2, 1, 1);
        g_signal_connect (chk, "toggled", G_CALLBACK (on_notify_type_toggled), pd);
        pd->notify_type_check[t] = chk;
    }
    props_update_notify_sensitivity (pd);

    gtk_box_pack_start (GTK_BOX (content), frame_bh, FALSE, FALSE, 0);

    /* señales */
    g_signal_connect (pd->square_check,      "toggled",       G_CALLBACK (on_square_toggled),      pd);
    g_signal_connect (pd->plugin_spin,       "value-changed", G_CALLBACK (on_plugin_size_changed),  pd);
    g_signal_connect (pd->panel_icon_check,  "toggled",       G_CALLBACK (on_panel_icon_toggled),   pd);
    g_signal_connect (pd->icon_spin,         "value-changed", G_CALLBACK (on_icon_size_changed),    pd);
    g_signal_connect (pd->popup_width_spin,  "value-changed", G_CALLBACK (on_popup_width_changed),  pd);
    g_signal_connect (pd->popup_height_spin, "value-changed", G_CALLBACK (on_popup_height_changed), pd);
    g_signal_connect (pd->popup_style_combo, "changed",       G_CALLBACK (on_popup_style_changed),  pd);
    g_signal_connect (pd->notify_check,      "toggled",       G_CALLBACK (on_notify_toggled),       pd);
    g_signal_connect (pd->hide_check,        "toggled",       G_CALLBACK (on_hide_toggled),         pd);
    g_signal_connect (pd->hide_other_check,  "toggled",       G_CALLBACK (on_hide_other_toggled),   pd);
    g_signal_connect (pd->notify_new_check,  "toggled",       G_CALLBACK (on_notify_new_toggled),   pd);
    g_signal_connect (pd->sounds_check,      "toggled",       G_CALLBACK (on_sounds_toggled),       pd);
    g_signal_connect (pd->show_input_check,  "toggled",       G_CALLBACK (on_show_input_toggled),   pd);
    g_signal_connect (dialog, "destroy",                      G_CALLBACK (on_props_dialog_destroy), pd);

    xfce_panel_plugin_block_menu (plugin);
    g_signal_connect_swapped (dialog, "response", G_CALLBACK (gtk_widget_destroy), dialog);
    g_signal_connect_swapped (dialog, "destroy",  G_CALLBACK (xfce_panel_plugin_unblock_menu), plugin);

    gtk_widget_show_all (dialog);
}

/* ---------- Acerca de ---------- */

#ifndef PACKAGE_VERSION
#define PACKAGE_VERSION "?"
#endif

static void
on_about (XfcePanelPlugin *plugin, RemovablePlugin *rp)
{
    (void) plugin;

    /* con el popup abierto él tiene el teclado (Wayland, wayland-notas §4.7) */
    if (rp->popup_open)
        popup_hide (rp->popup);

    if (rp->about_dialog) {
        gtk_window_present (GTK_WINDOW (rp->about_dialog));
        return;
    }

    static const gchar *authors[] = {
        "Tantin1 <juanperez33840@gmail.com>",
        NULL,
    };

    GtkWidget *dlg = gtk_about_dialog_new ();
    GtkAboutDialog *about = GTK_ABOUT_DIALOG (dlg);

    gtk_about_dialog_set_program_name   (about, _("Removable Devices"));
    gtk_about_dialog_set_version        (about, PACKAGE_VERSION);
    gtk_about_dialog_set_comments       (about,
        _("Mount and safely remove drives, phones and other USB devices"));
    gtk_about_dialog_set_logo_icon_name (about, "drive-removable-media");
    gtk_about_dialog_set_copyright      (about, "© 2025–2026 Tantin1");
    gtk_about_dialog_set_license_type   (about, GTK_LICENSE_GPL_2_0_ONLY);
    gtk_about_dialog_set_website        (about, "https://github.com/Tantin1/xfce-removable-plugin");
    gtk_about_dialog_set_website_label  (about, "github.com/Tantin1/xfce-removable-plugin");
    gtk_about_dialog_set_authors        (about, authors);

    gtk_window_set_icon_name (GTK_WINDOW (dlg), "drive-removable-media");

    rp->about_dialog = dlg;
    g_object_add_weak_pointer (G_OBJECT (dlg), (gpointer *) &rp->about_dialog);
    g_signal_connect (dlg, "response", G_CALLBACK (gtk_widget_destroy), NULL);

    gtk_widget_show (dlg);
    gtk_window_present (GTK_WINDOW (dlg));
}

/* ---------- callbacks del panel ---------- */

static gboolean
on_size_changed (XfcePanelPlugin *plugin, gint size, RemovablePlugin *rp)
{
    (void) plugin;
    (void) size;
    apply_config (rp);
    return TRUE;
}

static void
on_mode_changed (XfcePanelPlugin *plugin, XfcePanelPluginMode mode, RemovablePlugin *rp)
{
    (void) plugin;
    (void) mode;
    apply_config (rp);
}

static void
on_nrows_changed (XfcePanelPlugin *plugin, guint rows, RemovablePlugin *rp)
{
    (void) plugin;
    (void) rows;
    apply_config (rp);
}

static void
removable_plugin_free (XfcePanelPlugin *plugin, RemovablePlugin *rp)
{
    (void) plugin;
    if (rp->about_dialog)
        gtk_widget_destroy (rp->about_dialog);   /* el weak pointer lo deja en NULL */
    popup_destroy (rp->popup);
    if (rp->conn && rp->subscription)
        ud_unsubscribe (rp->conn, rp->subscription);
    if (rp->conn)
        g_object_unref (rp->conn);
    if (rp->button_css)
        g_object_unref (rp->button_css);
    if (rp->seen_storage)
        g_hash_table_destroy (rp->seen_storage);
    if (rp->seen_usb)
        g_hash_table_destroy (rp->seen_usb);
    if (rp->seen_phones)
        g_hash_table_destroy (rp->seen_phones);
    g_free (rp);
}

/* ---------- construcción del plugin ---------- */

static void
removable_plugin_construct (XfcePanelPlugin *plugin)
{
#ifdef ENABLE_NLS
    bindtextdomain (GETTEXT_PACKAGE, LOCALEDIR);
    bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
    /* sin textdomain(): ver wayland-notas §3.2 */
#endif

    notify_init ("xfce-removable-plugin");

    RemovablePlugin *rp = g_new0 (RemovablePlugin, 1);
    rp->plugin = plugin;

    load_config (rp);

    rp->conn = ud_dbus_connect ();

    rp->button = xfce_panel_create_toggle_button ();
    gtk_button_set_relief (GTK_BUTTON (rp->button), GTK_RELIEF_NONE);
    gtk_widget_set_tooltip_text (rp->button, _("Removable devices"));

    rp->icon = gtk_image_new_from_icon_name ("drive-removable-media-symbolic",
                                              GTK_ICON_SIZE_MENU);
    gtk_container_add (GTK_CONTAINER (rp->button), rp->icon);

    /* "chico": con el panel en varias filas entra en una sola celda, junto a
       otros íconos (como volumen, energía o la bandeja); con una fila no cambia nada */
    xfce_panel_plugin_set_small (plugin, TRUE);

    xfce_panel_plugin_add_action_widget (plugin, rp->button);
    gtk_container_add (GTK_CONTAINER (plugin), rp->button);
    gtk_widget_show_all (rp->button);

    apply_config (rp);

    rp->popup = popup_create (plugin, rp->button);
    popup_set_notify      (rp->popup, rp->show_notify);
    popup_set_first_run_hint (rp->popup, rp->first_run && rp->hide_if_empty);
    popup_set_style       (rp->popup, rp->popup_style);

    g_object_set_data (G_OBJECT (rp->popup->window), "closed-cb",   (gpointer) on_popup_closed);
    g_object_set_data (G_OBJECT (rp->popup->window), "closed-data",  rp);
    g_object_set_data (G_OBJECT (rp->popup->window), "show-input",
                       GINT_TO_POINTER (rp->show_input));

    g_signal_connect (rp->button, "clicked",
                      G_CALLBACK (plugin_popup_toggle), rp);

    if (rp->conn)
        rp->subscription = ud_subscribe (rp->conn,
                                         (UdSignalCallback) plugin_on_device_change, rp);

    /* snapshot inicial silencioso para no notificar lo ya enchufado */
    {
        UdScan *scan = plugin_scan (rp);
        update_snapshots (rp, scan, TRUE);
        apply_button_visibility (rp, scan);

        if (rp->first_run) {
            /* guardar ya la configuración: desde el próximo inicio no es "primera vez" */
            save_config (rp);

            /* por las dudas no abra el popup: avisar una sola vez, solo si en
               condiciones normales el botón estaría oculto */
            gboolean would_hide = rp->hide_if_empty && !scan->storage && !scan->phones &&
                                  (rp->hide_if_only_other || !scan->usb);
            if (would_hide && rp->show_notify)
                rp_notify (NULL, _("Removable Devices added"),
                           _("This button hides when nothing is connected and "
                             "reappears when you connect a device."),
                           "drive-removable-media");
        }
        ud_scan_free (scan);
    }

    xfce_panel_plugin_menu_show_configure (plugin);
    xfce_panel_plugin_menu_show_about (plugin);
    g_signal_connect (plugin, "configure-plugin", G_CALLBACK (on_configure_plugin), rp);
    g_signal_connect (plugin, "about",            G_CALLBACK (on_about),            rp);
    g_signal_connect (plugin, "size-changed",     G_CALLBACK (on_size_changed),     rp);
    g_signal_connect (plugin, "mode-changed",     G_CALLBACK (on_mode_changed),     rp);
    g_signal_connect (plugin, "nrows-changed",    G_CALLBACK (on_nrows_changed),    rp);
    g_signal_connect (plugin, "free-data",        G_CALLBACK (removable_plugin_free), rp);
}

/* ---------- registro ---------- */

XFCE_PANEL_PLUGIN_REGISTER (removable_plugin_construct);
