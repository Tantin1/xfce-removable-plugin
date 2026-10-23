#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <libxfce4panel/libxfce4panel.h>
#include <gtk/gtk.h>
#include <libnotify/notify.h>

#include "popup.h"
#include "ud-dbus.h"

/* _() = dgettext (GETTEXT_PACKAGE, …): en Wayland todos los plugins comparten
   el proceso del panel, y textdomain() cambiaría el idioma de los demás */
#include <glib/gi18n-lib.h>

/* ---------- defaults ---------- */

#define DEFAULT_SQUARE_BUTTON        FALSE
#define DEFAULT_PLUGIN_SIZE          24
#define DEFAULT_USE_PANEL_ICON       FALSE
#define DEFAULT_ICON_SIZE            16
#define DEFAULT_POPUP_WIDTH          400
#define DEFAULT_POPUP_HEIGHT         300
#define DEFAULT_SHOW_NOTIFY          TRUE
#define DEFAULT_OPEN_THUNAR          FALSE
#define DEFAULT_HIDE_IF_EMPTY        TRUE
#define DEFAULT_HIDE_IF_ONLY_OTHER   FALSE
#define DEFAULT_NOTIFY_NEW_DEVICE    TRUE

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
    gboolean         show_notify;
    gboolean         open_thunar;
    gboolean         hide_if_empty;
    gboolean         hide_if_only_other;
    gboolean         notify_new_device;

    /* snapshots para detectar enchufado/desenchufado */
    GHashTable      *seen_storage;   /* object_path -> label/device */
    GHashTable      *seen_usb;       /* syspath     -> name|type    */
    GHashTable      *seen_phones;    /* devfile/URI -> name         */

    /* CSS tamaño del botón */
    GtkCssProvider  *button_css;
} RemovablePlugin;

/* ---------- apply_config ---------- */

static void
apply_config (RemovablePlugin *rp)
{
    XfcePanelPlugin *plugin = rp->plugin;

    /* tamaño del botón via CSS */
    {
        gchar *css;
        gint   ps = xfce_panel_plugin_get_size (plugin);

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
   no se pisan. Si la instancia todavía no tiene el suyo, se lee el .ini
   compartido de versiones anteriores para no perder la configuración. */
#define LEGACY_CONFIG_PATH "xfce4/panel/xfce-removable-plugin.ini"

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

    if (!path)
        path = g_build_filename (g_get_user_config_dir (), LEGACY_CONFIG_PATH, NULL);

    rp->square_button       = DEFAULT_SQUARE_BUTTON;
    rp->plugin_size         = DEFAULT_PLUGIN_SIZE;
    rp->use_panel_icon      = DEFAULT_USE_PANEL_ICON;
    rp->icon_size           = DEFAULT_ICON_SIZE;
    rp->popup_width         = DEFAULT_POPUP_WIDTH;
    rp->popup_height        = DEFAULT_POPUP_HEIGHT;
    rp->show_notify         = DEFAULT_SHOW_NOTIFY;
    rp->open_thunar         = DEFAULT_OPEN_THUNAR;
    rp->hide_if_empty       = DEFAULT_HIDE_IF_EMPTY;
    rp->hide_if_only_other  = DEFAULT_HIDE_IF_ONLY_OTHER;
    rp->notify_new_device   = DEFAULT_NOTIFY_NEW_DEVICE;

    if (!g_key_file_load_from_file (kf, path, G_KEY_FILE_NONE, &err)) {
        g_clear_error (&err);
        goto out;
    }

    rp->square_button      = kf_get_bool (kf, "appearance", "square_button",      DEFAULT_SQUARE_BUTTON);
    rp->plugin_size        = kf_get_int  (kf, "appearance", "plugin_size",        DEFAULT_PLUGIN_SIZE);
    rp->use_panel_icon     = kf_get_bool (kf, "appearance", "use_panel_icon",     DEFAULT_USE_PANEL_ICON);
    rp->icon_size          = kf_get_int  (kf, "appearance", "icon_size",          DEFAULT_ICON_SIZE);
    rp->popup_width        = kf_get_int  (kf, "appearance", "popup_width",        DEFAULT_POPUP_WIDTH);
    rp->popup_height       = kf_get_int  (kf, "appearance", "popup_height",       DEFAULT_POPUP_HEIGHT);
    rp->show_notify        = kf_get_bool (kf, "behavior",   "show_notify",        DEFAULT_SHOW_NOTIFY);
    rp->open_thunar        = kf_get_bool (kf, "behavior",   "open_thunar",        DEFAULT_OPEN_THUNAR);
    rp->hide_if_empty      = kf_get_bool (kf, "behavior",   "hide_if_empty",      DEFAULT_HIDE_IF_EMPTY);
    rp->hide_if_only_other = kf_get_bool (kf, "behavior",   "hide_if_only_other", DEFAULT_HIDE_IF_ONLY_OTHER);
    rp->notify_new_device  = kf_get_bool (kf, "behavior",   "notify_new_device",  DEFAULT_NOTIFY_NEW_DEVICE);

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
    g_key_file_set_boolean  (kf, "behavior",   "show_notify",        rp->show_notify);
    g_key_file_set_boolean  (kf, "behavior",   "open_thunar",        rp->open_thunar);
    g_key_file_set_boolean  (kf, "behavior",   "hide_if_empty",      rp->hide_if_empty);
    g_key_file_set_boolean  (kf, "behavior",   "hide_if_only_other", rp->hide_if_only_other);
    g_key_file_set_boolean  (kf, "behavior",   "notify_new_device",  rp->notify_new_device);

    if (!g_key_file_save_to_file (kf, path, &err)) {
        g_warning ("xfce-removable-plugin: no se pudo guardar config: %s", err->message);
        g_error_free (err);
    }

    g_key_file_free (kf);
    g_free (path);
}

/* ---------- visibilidad del botón ---------- */

static void
apply_button_visibility (RemovablePlugin *rp, const UdScan *scan)
{
    /* los teléfonos cuentan como dispositivos "principales", no como "otros" */
    gboolean show = !rp->hide_if_empty ||
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
    UdScan *scan = ud_scan (rp->conn);
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

static void
notify_simple (const gchar *summary, const gchar *body, const gchar *icon)
{
    NotifyNotification *n = notify_notification_new (summary, body, icon);
    notify_notification_set_timeout (n, NOTIFY_EXPIRES_DEFAULT);
    notify_notification_show (n, NULL);
    g_object_unref (n);
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

    if (!silent && rp->notify_new_device) {
        /* enchufado: en new pero no en seen */
        GHashTableIter it;
        gpointer key, value;

        g_hash_table_iter_init (&it, new_storage);
        while (g_hash_table_iter_next (&it, &key, &value)) {
            if (!rp->seen_storage || !g_hash_table_contains (rp->seen_storage, key)) {
                gchar **parts = g_strsplit ((const gchar *) value, "|", 2);
                guint64 size  = parts[1] ? g_ascii_strtoull (parts[1], NULL, 10) : 0;
                gchar  *sz    = format_size_short (size);
                gchar  *body  = g_strdup_printf ("%s (%s)",
                                                 (parts[0] && *parts[0]) ? parts[0] : _("Unknown"),
                                                 sz);
                notify_simple (_("Removable device connected"), body, "drive-removable-media");
                g_free (body); g_free (sz); g_strfreev (parts);
            }
        }
        g_hash_table_iter_init (&it, new_usb);
        while (g_hash_table_iter_next (&it, &key, &value)) {
            if (!rp->seen_usb || !g_hash_table_contains (rp->seen_usb, key)) {
                gchar   **parts = g_strsplit ((const gchar *) value, "|", 2);
                UdUsbType type  = parts[1] ? (UdUsbType) g_ascii_strtoll (parts[1], NULL, 10)
                                           : UD_USB_OTHER;
                gchar    *body  = g_strdup_printf ("%s — %s",
                                                   (parts[0] && *parts[0]) ? parts[0] : _("Unknown"),
                                                   _(ud_usb_type_label (type)));
                notify_simple (_("USB device connected"), body, ud_usb_type_icon (type));
                g_free (body); g_strfreev (parts);
            }
        }

        g_hash_table_iter_init (&it, new_phones);
        while (g_hash_table_iter_next (&it, &key, &value)) {
            if (!rp->seen_phones || !g_hash_table_contains (rp->seen_phones, key))
                notify_simple (_("Phone or camera connected"),
                               *(const gchar *) value ? (const gchar *) value : _("Unknown"),
                               "phone");
        }

        /* desenchufado: en seen pero no en new */
        if (rp->seen_phones) {
            g_hash_table_iter_init (&it, rp->seen_phones);
            while (g_hash_table_iter_next (&it, &key, &value)) {
                if (!g_hash_table_contains (new_phones, key))
                    notify_simple (_("Phone or camera disconnected"),
                                   *(const gchar *) value ? (const gchar *) value : _("Unknown"),
                                   "phone");
            }
        }
        if (rp->seen_storage) {
            g_hash_table_iter_init (&it, rp->seen_storage);
            while (g_hash_table_iter_next (&it, &key, &value)) {
                if (!g_hash_table_contains (new_storage, key)) {
                    gchar **parts = g_strsplit ((const gchar *) value, "|", 2);
                    notify_simple (_("Removable device disconnected"),
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
                    notify_simple (_("USB device disconnected"),
                                   (parts[0] && *parts[0]) ? parts[0] : _("Unknown"),
                                   ud_usb_type_icon (type));
                    g_strfreev (parts);
                }
            }
        }
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
    UdScan *scan = ud_scan (rp->conn);

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
    GtkWidget       *notify_check;
    GtkWidget       *thunar_check;
    GtkWidget       *hide_check;
    GtkWidget       *hide_other_check;
    GtkWidget       *notify_new_check;
} PropsDialog;

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
on_thunar_toggled (GtkToggleButton *btn, PropsDialog *pd)
{
    pd->rp->open_thunar = gtk_toggle_button_get_active (btn);
    save_config (pd->rp);
    popup_set_open_thunar (pd->rp->popup, pd->rp->open_thunar);
}

static void
on_hide_toggled (GtkToggleButton *btn, PropsDialog *pd)
{
    pd->rp->hide_if_empty = gtk_toggle_button_get_active (btn);
    gtk_widget_set_sensitive (pd->hide_other_check, pd->rp->hide_if_empty);
    save_config (pd->rp);
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
    pd->popup_width_spin = gtk_spin_button_new_with_range (200, 800, 10);
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

    pd->thunar_check = gtk_check_button_new_with_label (_("Open file manager after mounting"));
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (pd->thunar_check), rp->open_thunar);
    gtk_grid_attach (GTK_GRID (grid_bh), pd->thunar_check, 0, row, 2, 1);
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

    pd->notify_new_check = gtk_check_button_new_with_label (
        _("Notify when devices are connected or disconnected"));
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (pd->notify_new_check), rp->notify_new_device);
    gtk_grid_attach (GTK_GRID (grid_bh), pd->notify_new_check, 0, row, 2, 1);

    gtk_box_pack_start (GTK_BOX (content), frame_bh, FALSE, FALSE, 0);

    /* señales */
    g_signal_connect (pd->square_check,      "toggled",       G_CALLBACK (on_square_toggled),      pd);
    g_signal_connect (pd->plugin_spin,       "value-changed", G_CALLBACK (on_plugin_size_changed),  pd);
    g_signal_connect (pd->panel_icon_check,  "toggled",       G_CALLBACK (on_panel_icon_toggled),   pd);
    g_signal_connect (pd->icon_spin,         "value-changed", G_CALLBACK (on_icon_size_changed),    pd);
    g_signal_connect (pd->popup_width_spin,  "value-changed", G_CALLBACK (on_popup_width_changed),  pd);
    g_signal_connect (pd->popup_height_spin, "value-changed", G_CALLBACK (on_popup_height_changed), pd);
    g_signal_connect (pd->notify_check,      "toggled",       G_CALLBACK (on_notify_toggled),       pd);
    g_signal_connect (pd->thunar_check,      "toggled",       G_CALLBACK (on_thunar_toggled),       pd);
    g_signal_connect (pd->hide_check,        "toggled",       G_CALLBACK (on_hide_toggled),         pd);
    g_signal_connect (pd->hide_other_check,  "toggled",       G_CALLBACK (on_hide_other_toggled),   pd);
    g_signal_connect (pd->notify_new_check,  "toggled",       G_CALLBACK (on_notify_new_toggled),   pd);
    g_signal_connect (dialog, "destroy",                      G_CALLBACK (on_props_dialog_destroy), pd);

    xfce_panel_plugin_block_menu (plugin);
    g_signal_connect_swapped (dialog, "response", G_CALLBACK (gtk_widget_destroy), dialog);
    g_signal_connect_swapped (dialog, "destroy",  G_CALLBACK (xfce_panel_plugin_unblock_menu), plugin);

    gtk_widget_show_all (dialog);
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
removable_plugin_free (XfcePanelPlugin *plugin, RemovablePlugin *rp)
{
    (void) plugin;
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

    xfce_panel_plugin_add_action_widget (plugin, rp->button);
    gtk_container_add (GTK_CONTAINER (plugin), rp->button);
    gtk_widget_show_all (rp->button);

    apply_config (rp);

    rp->popup = popup_create (plugin, rp->button);
    popup_set_notify      (rp->popup, rp->show_notify);
    popup_set_open_thunar (rp->popup, rp->open_thunar);

    g_object_set_data (G_OBJECT (rp->popup->window), "closed-cb",   (gpointer) on_popup_closed);
    g_object_set_data (G_OBJECT (rp->popup->window), "closed-data",  rp);

    g_signal_connect (rp->button, "clicked",
                      G_CALLBACK (plugin_popup_toggle), rp);

    if (rp->conn)
        rp->subscription = ud_subscribe (rp->conn,
                                         (UdSignalCallback) plugin_on_device_change, rp);

    /* snapshot inicial silencioso para no notificar lo ya enchufado */
    {
        UdScan *scan = ud_scan (rp->conn);
        update_snapshots (rp, scan, TRUE);
        apply_button_visibility (rp, scan);
        ud_scan_free (scan);
    }

    xfce_panel_plugin_menu_show_configure (plugin);
    g_signal_connect (plugin, "configure-plugin", G_CALLBACK (on_configure_plugin), rp);
    g_signal_connect (plugin, "size-changed",     G_CALLBACK (on_size_changed),     rp);
    g_signal_connect (plugin, "mode-changed",     G_CALLBACK (on_mode_changed),     rp);
    g_signal_connect (plugin, "free-data",        G_CALLBACK (removable_plugin_free), rp);
}

/* ---------- registro ---------- */

XFCE_PANEL_PLUGIN_REGISTER (removable_plugin_construct);
