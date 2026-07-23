#include "popup.h"
#include "ud-dbus.h"
#include <gdk/gdk.h>
#include <libnotify/notify.h>

#ifdef ENABLE_NLS
#include <libintl.h>
#define _(x) gettext(x)
#else
#define _(x) (x)
#endif

#define POPUP_WIDTH  400
#define POPUP_HEIGHT 300

/* ---------- formato de tamaño ---------- */

static gchar *
format_size (guint64 bytes)
{
    if (bytes >= (guint64) 1e9)
        return g_strdup_printf ("%.1f GB", bytes / 1e9);
    if (bytes >= (guint64) 1e6)
        return g_strdup_printf ("%.0f MB", bytes / 1e6);
    return g_strdup_printf ("%.0f KB", bytes / 1e3);
}

/* ---------- posicionamiento ---------- */

static void
position_popup (RemovablePopup *popup, GtkWidget *button)
{
    GdkWindow      *gdk_win;
    GdkMonitor     *monitor;
    GdkRectangle    workarea;
    GtkRequisition  preferred;
    gint bx, by, bw, bh, pw, ph, x, y;

    gdk_win = gtk_widget_get_window (button);
    gdk_window_get_origin (gdk_win, &bx, &by);
    bw = gtk_widget_get_allocated_width  (button);
    bh = gtk_widget_get_allocated_height (button);

    gtk_widget_get_preferred_size (popup->window, NULL, &preferred);
    pw = preferred.width;
    ph = preferred.height;

    monitor = gdk_display_get_monitor_at_point (
                  gtk_widget_get_display (button), bx + bw / 2, by + bh / 2);
    gdk_monitor_get_workarea (monitor, &workarea);

    x = bx;
    y = by + bh;

    if (x + pw > workarea.x + workarea.width)
        x = workarea.x + workarea.width - pw;

    if (y + ph > workarea.y + workarea.height)
        y = by - ph;

    gtk_window_move (GTK_WINDOW (popup->window), x, y);
}

/* ---------- click fuera del popup ---------- */

static gboolean
on_focus_out (GtkWidget *widget, GdkEventFocus *ev, RemovablePopup *popup)
{
    (void) widget; (void) ev;
    popup_hide (popup);
    return FALSE;
}

static gboolean
on_button_press (GtkWidget *widget, GdkEventButton *ev, RemovablePopup *popup)
{
    (void) widget;
    GtkAllocation alloc;
    gtk_widget_get_allocation (popup->window, &alloc);
    gint wx, wy;
    gtk_window_get_position (GTK_WINDOW (popup->window), &wx, &wy);
    if (ev->x_root < wx || ev->x_root > wx + alloc.width ||
        ev->y_root < wy || ev->y_root > wy + alloc.height)
        popup_hide (popup);
    return FALSE;
}

static gboolean
on_key_press (GtkWidget *widget, GdkEventKey *ev, RemovablePopup *popup)
{
    (void) widget;
    if (ev->keyval == GDK_KEY_Escape)
        popup_hide (popup);
    return FALSE;
}

/* prototipo adelantado */
static void populate_content (RemovablePopup *popup, GDBusConnection *conn);

/* ---------- datos por fila de partición ---------- */

typedef struct {
    RemovablePopup  *popup;
    GDBusConnection *conn;
    gchar           *object_path;
    gchar           *drive_path;
    gboolean         mounted;
    GtkWidget       *btn_action;
} RowData;

static void
row_data_free (gpointer data)
{
    RowData *rd = data;
    g_free (rd->object_path);
    g_free (rd->drive_path);
    g_free (rd);
}

/* ---------- datos para el botón Eject ---------- */

typedef struct {
    RemovablePopup  *popup;
    GDBusConnection *conn;
    gchar           *drive_path;
    GSList          *partitions;
} EjectData;

static void
eject_data_free (gpointer data)
{
    EjectData *ed = data;
    g_free (ed->drive_path);
    g_slist_free_full (ed->partitions, g_free);
    g_free (ed);
}

/* ---------- datos para el botón expand ---------- */

typedef struct {
    RemovablePopup *popup;
    gchar          *key;        /* drive_path o "__other__" */
    GtkWidget      *parts_box;
    GtkWidget      *arrow;
    gboolean        expanded;
} ExpandData;

static void
expand_data_free (gpointer data)
{
    ExpandData *ed = data;
    g_free (ed->key);
    g_free (ed);
}

/* aplica visualmente el estado expandido/colapsado */
static void
expander_apply (ExpandData *ed)
{
    if (ed->expanded) {
        gtk_container_foreach (GTK_CONTAINER (ed->parts_box),
                               (GtkCallback) gtk_widget_show_all, NULL);
        gtk_widget_show (ed->parts_box);
        gtk_widget_queue_resize (ed->parts_box);

        gtk_image_set_from_icon_name (GTK_IMAGE (ed->arrow),
                                      "pan-down-symbolic",
                                      GTK_ICON_SIZE_MENU);
    } else {
        gtk_widget_hide (ed->parts_box);
        gtk_image_set_from_icon_name (GTK_IMAGE (ed->arrow),
                                      "pan-end-symbolic",
                                      GTK_ICON_SIZE_MENU);
    }
}

static gboolean
on_expand_clicked (GtkWidget *btn, GdkEventButton *event, ExpandData *ed)
{
    (void) btn; (void) event;
    ed->expanded = !ed->expanded;

    /* recordar el estado para que un refresco no lo colapse */
    if (ed->popup && ed->key) {
        if (ed->expanded)
            g_hash_table_add (ed->popup->expanded, g_strdup (ed->key));
        else
            g_hash_table_remove (ed->popup->expanded, ed->key);
    }

    expander_apply (ed);
    return FALSE;
}

/* ---------- notificaciones ---------- */

static void
notify_send (const gchar *summary, const gchar *body, const gchar *icon)
{
    NotifyNotification *n = notify_notification_new (summary, body, icon);
    notify_notification_set_timeout (n, NOTIFY_EXPIRES_DEFAULT);
    notify_notification_show (n, NULL);
    g_object_unref (n);
}

/* ---------- callbacks de botones ---------- */

static void
on_mount_clicked (GtkWidget *btn, RowData *rd)
{
    gtk_widget_set_sensitive (btn, FALSE);
    gtk_button_set_label (GTK_BUTTON (btn), _("Mounting…"));
    while (gtk_events_pending ()) gtk_main_iteration_do (FALSE);

    gchar *mp = ud_mount (rd->conn, rd->object_path);
    if (mp) {
        if (rd->popup->show_notify)
            notify_send (_("Device mounted"), mp, "drive-removable-media");
        if (rd->popup->open_thunar) {
            gchar *quoted = g_shell_quote (mp);
            gchar *cmd    = g_strdup_printf ("thunar %s", quoted);
            g_spawn_command_line_async (cmd, NULL);
            g_free (cmd);
            g_free (quoted);
        }
        g_free (mp);
    } else {
        if (rd->popup->show_notify)
            notify_send (_("Mount failed"), _("Could not mount the device."), "dialog-error");
        gtk_button_set_label (GTK_BUTTON (btn), _("Mount"));
        gtk_widget_set_sensitive (btn, TRUE);
    }
}

static void
on_unmount_clicked (GtkWidget *btn, RowData *rd)
{
    gtk_widget_set_sensitive (btn, FALSE);
    gtk_button_set_label (GTK_BUTTON (btn), _("Unmounting…"));
    while (gtk_events_pending ()) gtk_main_iteration_do (FALSE);

    gboolean ok = ud_unmount (rd->conn, rd->object_path);
    if (ok) {
        if (rd->popup->show_notify)
            notify_send (_("Safe to remove"), _("You can now unplug the device."), "drive-removable-media");
    } else {
        if (rd->popup->show_notify)
            notify_send (_("Unmount failed"), _("Could not unmount the device."), "dialog-error");
        gtk_button_set_label (GTK_BUTTON (btn), _("Unmount"));
        gtk_widget_set_sensitive (btn, TRUE);
    }
}

static void
on_eject_clicked (GtkWidget *btn, EjectData *ed)
{
    gtk_widget_set_sensitive (btn, FALSE);
    while (gtk_events_pending ()) gtk_main_iteration_do (FALSE);

    if (ed->popup->show_notify)
        notify_send (_("Ejecting…"), _("Please wait, do not unplug."), "drive-removable-media");
    while (gtk_events_pending ()) gtk_main_iteration_do (FALSE);

    for (GSList *l = ed->partitions; l; l = l->next)
        ud_unmount (ed->conn, (const gchar *) l->data);

    ud_eject (ed->conn, ed->drive_path);

    if (ed->popup->show_notify)
        notify_send (_("Device ejected"), _("Safe to unplug."), "drive-removable-media");

    populate_content (ed->popup, ed->conn);
}

/* ---------- fila de partición (indentada) ---------- */

static GtkWidget *
build_partition_row (RemovablePopup *popup, GDBusConnection *conn, UdDevice *dev)
{
    RowData *rd     = g_new0 (RowData, 1);
    rd->popup       = popup;
    rd->conn        = conn;
    rd->object_path = g_strdup (dev->object_path);
    rd->drive_path  = g_strdup (dev->drive_path);
    rd->mounted     = dev->mounted;

    GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_start  (row, 32);
    gtk_widget_set_margin_end    (row, 12);
    gtk_widget_set_margin_top    (row, 4);
    gtk_widget_set_margin_bottom (row, 4);
    g_object_set_data_full (G_OBJECT (row), "row-data", rd, row_data_free);

    GtkWidget *icon = gtk_image_new_from_icon_name (
        "drive-harddisk-symbolic", GTK_ICON_SIZE_MENU);
    gtk_box_pack_start (GTK_BOX (row), icon, FALSE, FALSE, 0);

    GtkWidget *info_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_hexpand (info_box, TRUE);

    const gchar *name = (dev->label && *dev->label) ? dev->label : dev->device;
    GtkWidget *name_label = gtk_label_new (name);
    gtk_label_set_xalign (GTK_LABEL (name_label), 0.0);
    gtk_label_set_ellipsize (GTK_LABEL (name_label), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars (GTK_LABEL (name_label), 24);

    gchar *size_str = format_size (dev->size);
    gchar *detail;
    if (dev->mounted && dev->mount_point)
        detail = g_strdup_printf ("%s — %s", size_str, dev->mount_point);
    else
        detail = g_strdup (size_str);
    g_free (size_str);

    GtkWidget *detail_label = gtk_label_new (detail);
    g_free (detail);
    gtk_label_set_xalign (GTK_LABEL (detail_label), 0.0);
    gtk_label_set_ellipsize (GTK_LABEL (detail_label), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars (GTK_LABEL (detail_label), 24);
    gtk_style_context_add_class (gtk_widget_get_style_context (detail_label), "dim-label");

    gtk_box_pack_start (GTK_BOX (info_box), name_label,   FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (info_box), detail_label, FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (row), info_box, TRUE, TRUE, 0);

    GtkWidget *btn_action;
    if (dev->mounted) {
        btn_action = gtk_button_new_with_label (_("Unmount"));
        g_signal_connect (btn_action, "clicked", G_CALLBACK (on_unmount_clicked), rd);
    } else {
        btn_action = gtk_button_new_with_label (_("Mount"));
        g_signal_connect (btn_action, "clicked", G_CALLBACK (on_mount_clicked), rd);
    }
    rd->btn_action = btn_action;
    gtk_box_pack_start (GTK_BOX (row), btn_action, FALSE, FALSE, 0);

    return row;
}

/* ---------- fila de encabezado de disco ---------- */

static GtkWidget *
build_drive_header (RemovablePopup *popup, GDBusConnection *conn,
                    const gchar *drive_path, GSList *partitions,
                    GtkWidget *parts_box)
{
    GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_start  (row, 12);
    gtk_widget_set_margin_end    (row, 12);
    gtk_widget_set_margin_top    (row, 8);
    gtk_widget_set_margin_bottom (row, 4);

    /* flecha de estado — solo visual, el click va en el event_box de populate_content */
    GtkWidget *arrow = gtk_image_new_from_icon_name (
        "pan-end-symbolic", GTK_ICON_SIZE_MENU);
    g_object_set_data (G_OBJECT (row), "arrow", arrow);
    gtk_box_pack_start (GTK_BOX (row), arrow, FALSE, FALSE, 0);

    /* ícono del disco */
    GtkWidget *icon = gtk_image_new_from_icon_name (
        "drive-removable-media-symbolic", GTK_ICON_SIZE_LARGE_TOOLBAR);
    gtk_box_pack_start (GTK_BOX (row), icon, FALSE, FALSE, 0);

    /* nombre del disco */
    gchar *drive_name = NULL;
    GError   *err = NULL;
    GVariant *ret = g_dbus_connection_call_sync (
        conn, UD_BUS_NAME, drive_path,
        "org.freedesktop.DBus.Properties", "Get",
        g_variant_new ("(ss)", UD_DRIVE_IFACE, "Model"),
        G_VARIANT_TYPE ("(v)"),
        G_DBUS_CALL_FLAGS_NONE, 2000, NULL, &err);
    if (ret) {
        GVariant *val;
        g_variant_get (ret, "(v)", &val);
        const gchar *model = g_variant_get_string (val, NULL);
        if (model && *model)
            drive_name = g_strdup (model);
        g_variant_unref (val);
        g_variant_unref (ret);
    } else {
        g_clear_error (&err);
    }

    if (!drive_name && partitions) {
        UdDevice *first = partitions->data;
        if (first->device && *first->device) {
            gchar *d = g_strdup (first->device);
            gsize len = strlen (d);
            while (len > 0 && g_ascii_isdigit (d[len-1])) { d[--len] = '\0'; }
            drive_name = d;
        }
    }
    if (!drive_name)
        drive_name = g_strdup (_("Removable disk"));

    GtkWidget *name_label = gtk_label_new (drive_name);
    g_free (drive_name);
    gtk_label_set_xalign (GTK_LABEL (name_label), 0.0);
    gtk_label_set_ellipsize (GTK_LABEL (name_label), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars (GTK_LABEL (name_label), 26);
    gtk_widget_set_hexpand (name_label, TRUE);

    PangoAttrList *attrs = pango_attr_list_new ();
    pango_attr_list_insert (attrs, pango_attr_weight_new (PANGO_WEIGHT_BOLD));
    gtk_label_set_attributes (GTK_LABEL (name_label), attrs);
    pango_attr_list_unref (attrs);
    gtk_box_pack_start (GTK_BOX (row), name_label, TRUE, TRUE, 0);

    /* botón Eject */
    gboolean ejectable = FALSE;
    GSList *mounted_paths = NULL;
    for (GSList *l = partitions; l; l = l->next) {
        UdDevice *dev = l->data;
        if (dev->ejectable) ejectable = TRUE;
        if (dev->mounted)
            mounted_paths = g_slist_append (mounted_paths, g_strdup (dev->object_path));
    }

    if (ejectable) {
        EjectData *ejd  = g_new0 (EjectData, 1);
        ejd->popup      = popup;
        ejd->conn       = conn;
        ejd->drive_path = g_strdup (drive_path);
        ejd->partitions = mounted_paths;

        GtkWidget *btn_eject = gtk_button_new_from_icon_name (
            "media-eject-symbolic", GTK_ICON_SIZE_BUTTON);
        gtk_widget_set_tooltip_text (btn_eject, _("Eject"));
        g_object_set_data_full (G_OBJECT (btn_eject), "eject-data", ejd, eject_data_free);
        g_signal_connect (btn_eject, "clicked", G_CALLBACK (on_eject_clicked), ejd);
        gtk_box_pack_start (GTK_BOX (row), btn_eject, FALSE, FALSE, 0);
    } else {
        g_slist_free_full (mounted_paths, g_free);
    }

    return row;
}

/* ---------- fila de dispositivo USB no-almacenamiento ---------- */

static const gchar *
icon_for_usb_type (const gchar *type)
{
    if (!type)                                          return "drive-removable-media";
    if (g_str_equal (type, "Printer"))                  return "printer";
    if (g_str_equal (type, "Audio device"))             return "audio-card";
    if (g_str_equal (type, "Camera / Webcam"))          return "camera-web";
    if (g_str_equal (type, "Gamepad / Joystick"))       return "input-gaming";
    return "drive-removable-media";
}

static GtkWidget *
build_usb_device_row (UdUsbDevice *dev)
{
    GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_start  (row, 32);
    gtk_widget_set_margin_end    (row, 12);
    gtk_widget_set_margin_top    (row, 4);
    gtk_widget_set_margin_bottom (row, 4);

    GtkWidget *icon = gtk_image_new_from_icon_name (
        icon_for_usb_type (dev->type), GTK_ICON_SIZE_MENU);
    gtk_box_pack_start (GTK_BOX (row), icon, FALSE, FALSE, 0);

    GtkWidget *info_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_hexpand (info_box, TRUE);

    GtkWidget *name_label = gtk_label_new (dev->name);
    gtk_label_set_xalign (GTK_LABEL (name_label), 0.0);
    gtk_label_set_ellipsize (GTK_LABEL (name_label), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars (GTK_LABEL (name_label), 32);

    GtkWidget *type_label = gtk_label_new (dev->type);
    gtk_label_set_xalign (GTK_LABEL (type_label), 0.0);
    gtk_style_context_add_class (gtk_widget_get_style_context (type_label), "dim-label");

    gtk_box_pack_start (GTK_BOX (info_box), name_label, FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (info_box), type_label, FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (row), info_box, TRUE, TRUE, 0);

    return row;
}

/* ---------- poblar contenido ---------- */

static void
populate_content (RemovablePopup *popup, GDBusConnection *conn)
{
    GList *kids = gtk_container_get_children (GTK_CONTAINER (popup->content_box));
    for (GList *k = kids; k; k = k->next)
        gtk_widget_destroy (k->data);
    g_list_free (kids);

    GSList *devices = ud_get_devices (conn);

    if (!devices) {
        GSList *usb_devs = ud_get_usb_devices ();
        if (!usb_devs) {
            GtkWidget *label = gtk_label_new (_("No removable devices"));
            gtk_widget_set_margin_top    (label, 16);
            gtk_widget_set_margin_bottom (label, 16);
            gtk_widget_set_margin_start  (label, 16);
            gtk_widget_set_margin_end    (label, 16);
            gtk_box_pack_start (GTK_BOX (popup->content_box), label, TRUE, TRUE, 0);
            gtk_widget_show (label);
            return;
        }
        /* hay otros dispositivos aunque no haya almacenamiento */
        GtkWidget *usb_box2 = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
        for (GSList *l = usb_devs; l; l = l->next) {
            GtkWidget *row = build_usb_device_row (l->data);
            gtk_box_pack_start (GTK_BOX (usb_box2), row, FALSE, FALSE, 0);
        }
        ud_usb_device_list_free (usb_devs);

        GtkWidget *hdr2 = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_set_margin_start  (hdr2, 12);
        gtk_widget_set_margin_end    (hdr2, 12);
        gtk_widget_set_margin_top    (hdr2, 8);
        gtk_widget_set_margin_bottom (hdr2, 4);

        GtkWidget *arrow2 = gtk_image_new_from_icon_name (
            "pan-end-symbolic", GTK_ICON_SIZE_MENU);

        ExpandData *ed2 = g_new0 (ExpandData, 1);
        ed2->popup      = popup;
        ed2->key        = g_strdup ("__other__");
        ed2->parts_box  = usb_box2;
        ed2->arrow      = arrow2;
        ed2->expanded   = g_hash_table_contains (popup->expanded, "__other__");

        GtkWidget *hdr_icon2  = gtk_image_new_from_icon_name (
            "emblem-system-symbolic", GTK_ICON_SIZE_LARGE_TOOLBAR);
        GtkWidget *hdr_label2 = gtk_label_new (_("Other devices"));
        gtk_label_set_xalign (GTK_LABEL (hdr_label2), 0.0);
        gtk_widget_set_hexpand (hdr_label2, TRUE);

        PangoAttrList *attrs2 = pango_attr_list_new ();
        pango_attr_list_insert (attrs2, pango_attr_weight_new (PANGO_WEIGHT_BOLD));
        gtk_label_set_attributes (GTK_LABEL (hdr_label2), attrs2);
        pango_attr_list_unref (attrs2);

        gtk_box_pack_start (GTK_BOX (hdr2), arrow2,     FALSE, FALSE, 0);
        gtk_box_pack_start (GTK_BOX (hdr2), hdr_icon2,  FALSE, FALSE, 0);
        gtk_box_pack_start (GTK_BOX (hdr2), hdr_label2, TRUE,  TRUE,  0);

        GtkWidget *hdr_event2 = gtk_event_box_new ();
        gtk_container_add (GTK_CONTAINER (hdr_event2), hdr2);
        g_object_set_data_full (G_OBJECT (hdr_event2), "expand-data", ed2, expand_data_free);
        g_signal_connect (hdr_event2, "button-press-event", G_CALLBACK (on_expand_clicked), ed2);
        gtk_box_pack_start (GTK_BOX (popup->content_box), hdr_event2, FALSE, FALSE, 0);
        gtk_widget_show_all (hdr_event2);

        gtk_widget_set_no_show_all (usb_box2, TRUE);
        gtk_box_pack_start (GTK_BOX (popup->content_box), usb_box2, FALSE, FALSE, 0);
        if (ed2->expanded)
            expander_apply (ed2);
        return;
    }

    /* agrupar por drive_path */
    GHashTable *drive_map   = g_hash_table_new (g_str_hash, g_str_equal);
    GSList     *drive_order = NULL;

    for (GSList *l = devices; l; l = l->next) {
        UdDevice *dev = l->data;
        if (!g_hash_table_contains (drive_map, dev->drive_path)) {
            g_hash_table_insert (drive_map, dev->drive_path, NULL);
            drive_order = g_slist_append (drive_order, dev->drive_path);
        }
        GSList *parts = g_hash_table_lookup (drive_map, dev->drive_path);
        parts = g_slist_append (parts, dev);
        g_hash_table_replace (drive_map, dev->drive_path, parts);
    }

    gboolean first_drive = TRUE;
    for (GSList *d = drive_order; d; d = d->next) {
        const gchar *drive_path = d->data;
        GSList *parts = g_hash_table_lookup (drive_map, drive_path);

        if (!first_drive) {
            GtkWidget *sep = gtk_separator_new (GTK_ORIENTATION_HORIZONTAL);
            gtk_box_pack_start (GTK_BOX (popup->content_box), sep, FALSE, FALSE, 0);
            gtk_widget_show (sep);
        }
        first_drive = FALSE;

        /* parts_box: contenedor de particiones, oculto por defecto */
        GtkWidget *parts_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
        /* NO se llama show() sobre parts_box aquí */

        /* encabezado envuelto en event_box para capturar click en toda la fila */
        GtkWidget *header = build_drive_header (popup, conn, drive_path, parts, parts_box);

        ExpandData *ed = g_new0 (ExpandData, 1);
        ed->popup      = popup;
        ed->key        = g_strdup (drive_path);
        ed->parts_box  = parts_box;
        ed->arrow      = g_object_get_data (G_OBJECT (header), "arrow");
        ed->expanded   = g_hash_table_contains (popup->expanded, drive_path);

        GtkWidget *event_box = gtk_event_box_new ();
        gtk_container_add (GTK_CONTAINER (event_box), header);
        g_object_set_data_full (G_OBJECT (event_box), "expand-data", ed, expand_data_free);
        g_signal_connect (event_box, "button-press-event", G_CALLBACK (on_expand_clicked), ed);
        gtk_box_pack_start (GTK_BOX (popup->content_box), event_box, FALSE, FALSE, 0);
        gtk_widget_show_all (event_box);

        /* filas de partición dentro de parts_box */
        for (GSList *p = parts; p; p = p->next) {
            GtkWidget *part_row = build_partition_row (popup, conn, p->data);
            gtk_box_pack_start (GTK_BOX (parts_box), part_row, FALSE, FALSE, 0);
        }

        gtk_widget_set_no_show_all (parts_box, TRUE);
        gtk_box_pack_start (GTK_BOX (popup->content_box), parts_box, FALSE, FALSE, 0);
        if (ed->expanded)
            expander_apply (ed);

        g_slist_free (parts);
    }

    g_slist_free (drive_order);
    g_hash_table_destroy (drive_map);
    ud_device_list_free (devices);

    /* --- sección "Other devices" con expand --- */
    GSList *usb_devs = ud_get_usb_devices ();
    if (usb_devs) {
        GtkWidget *sep = gtk_separator_new (GTK_ORIENTATION_HORIZONTAL);
        gtk_box_pack_start (GTK_BOX (popup->content_box), sep, FALSE, FALSE, 0);
        gtk_widget_show (sep);

        /* usb_box: contenedor de filas, oculto por defecto */
        GtkWidget *usb_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
        for (GSList *l = usb_devs; l; l = l->next) {
            GtkWidget *row = build_usb_device_row (l->data);
            gtk_box_pack_start (GTK_BOX (usb_box), row, FALSE, FALSE, 0);
        }
        ud_usb_device_list_free (usb_devs);

        /* encabezado "Other devices" con flecha */
        GtkWidget *hdr = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_set_margin_start  (hdr, 12);
        gtk_widget_set_margin_end    (hdr, 12);
        gtk_widget_set_margin_top    (hdr, 8);
        gtk_widget_set_margin_bottom (hdr, 4);

        GtkWidget *arrow = gtk_image_new_from_icon_name (
            "pan-end-symbolic", GTK_ICON_SIZE_MENU);

        ExpandData *ed_usb = g_new0 (ExpandData, 1);
        ed_usb->popup      = popup;
        ed_usb->key        = g_strdup ("__other__");
        ed_usb->parts_box  = usb_box;
        ed_usb->arrow      = arrow;
        ed_usb->expanded   = g_hash_table_contains (popup->expanded, "__other__");

        GtkWidget *hdr_icon = gtk_image_new_from_icon_name (
            "emblem-system-symbolic", GTK_ICON_SIZE_LARGE_TOOLBAR);
        GtkWidget *hdr_label = gtk_label_new (_("Other devices"));
        gtk_label_set_xalign (GTK_LABEL (hdr_label), 0.0);
        gtk_widget_set_hexpand (hdr_label, TRUE);

        PangoAttrList *attrs = pango_attr_list_new ();
        pango_attr_list_insert (attrs, pango_attr_weight_new (PANGO_WEIGHT_BOLD));
        gtk_label_set_attributes (GTK_LABEL (hdr_label), attrs);
        pango_attr_list_unref (attrs);

        gtk_box_pack_start (GTK_BOX (hdr), arrow,     FALSE, FALSE, 0);
        gtk_box_pack_start (GTK_BOX (hdr), hdr_icon,  FALSE, FALSE, 0);
        gtk_box_pack_start (GTK_BOX (hdr), hdr_label, TRUE,  TRUE,  0);

        GtkWidget *hdr_event = gtk_event_box_new ();
        gtk_container_add (GTK_CONTAINER (hdr_event), hdr);
        g_object_set_data_full (G_OBJECT (hdr_event), "expand-data", ed_usb, expand_data_free);
        g_signal_connect (hdr_event, "button-press-event", G_CALLBACK (on_expand_clicked), ed_usb);
        gtk_box_pack_start (GTK_BOX (popup->content_box), hdr_event, FALSE, FALSE, 0);
        gtk_widget_show_all (hdr_event);

        gtk_widget_set_no_show_all (usb_box, TRUE);
        gtk_box_pack_start (GTK_BOX (popup->content_box), usb_box, FALSE, FALSE, 0);
        if (ed_usb->expanded)
            expander_apply (ed_usb);
    }
}

/* ---------- crear popup ---------- */

RemovablePopup *
popup_create (XfcePanelPlugin *plugin, GtkWidget *button)
{
    (void) plugin;

    RemovablePopup *popup = g_new0 (RemovablePopup, 1);

    GtkWidget *win = gtk_window_new (GTK_WINDOW_TOPLEVEL);
    gtk_window_set_type_hint         (GTK_WINDOW (win), GDK_WINDOW_TYPE_HINT_POPUP_MENU);
    gtk_window_set_keep_above        (GTK_WINDOW (win), TRUE);
    gtk_window_set_skip_taskbar_hint (GTK_WINDOW (win), TRUE);
    gtk_window_set_skip_pager_hint   (GTK_WINDOW (win), TRUE);
    gtk_window_set_resizable         (GTK_WINDOW (win), FALSE);
    gtk_window_set_decorated         (GTK_WINDOW (win), FALSE);
    gtk_container_set_border_width   (GTK_CONTAINER (win), 1);

    GtkWidget *scroll = gtk_scrolled_window_new (NULL, NULL);
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll),
                                    GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request (scroll, POPUP_WIDTH, POPUP_HEIGHT);
    gtk_container_add (GTK_CONTAINER (win), scroll);

    GtkWidget *content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add (GTK_CONTAINER (scroll), content);

    popup->window       = win;
    popup->content_box  = content;
    popup->scroll       = scroll;
    popup->button       = button;
    popup->show_notify  = TRUE;
    popup->open_thunar  = FALSE;
    popup->popup_width  = POPUP_WIDTH;
    popup->popup_height = POPUP_HEIGHT;
    popup->expanded     = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                 g_free, NULL);

    g_signal_connect (win, "key-press-event", G_CALLBACK (on_key_press), popup);

    return popup;
}

/* ---------- mostrar popup ---------- */

void
popup_show (RemovablePopup *popup, XfcePanelPlugin *plugin,
            GtkWidget *button, GDBusConnection *conn,
            gint popup_width, gint popup_height)
{
    (void) plugin;

    /* aplicar tamaño configurado (bug: antes se ignoraba) */
    popup->popup_width  = (popup_width  > 0) ? popup_width  : POPUP_WIDTH;
    popup->popup_height = (popup_height > 0) ? popup_height : POPUP_HEIGHT;

    g_object_set_data (G_OBJECT (popup->window), "conn", conn);

    populate_content (popup, conn);

    gtk_widget_set_size_request (popup->window, popup->popup_width, -1);
    gtk_widget_set_size_request (popup->scroll, popup->popup_width, popup->popup_height);

    /* mostrar ventana, scroll y content_box — pero NO sus hijos (populate_content
       ya los mostró selectivamente) */
    gtk_widget_show_all (popup->window);

    position_popup (popup, button);

    GdkDisplay      *display = gtk_widget_get_display (popup->window);
    GdkSeat         *seat    = gdk_display_get_default_seat (display);
    GdkGrabStatus    grab_status;
    grab_status = gdk_seat_grab (seat,
                                 gtk_widget_get_window (popup->window),
                                 GDK_SEAT_CAPABILITY_ALL,
                                 TRUE, NULL, NULL, NULL, NULL);

    if (grab_status == GDK_GRAB_SUCCESS) {
        g_debug ("xfce-removable-plugin: grab OK, usando button-press");
        popup->press_handler = g_signal_connect (popup->window, "button-press-event",
                                                 G_CALLBACK (on_button_press), popup);
    } else {
        g_debug ("xfce-removable-plugin: grab FALLÓ, usando focus-out");
        popup->press_handler = g_signal_connect (popup->window, "focus-out-event",
                                                 G_CALLBACK (on_focus_out), popup);
    }
}

/* ---------- ocultar / destruir ---------- */

void
popup_hide (RemovablePopup *popup)
{
    GdkDisplay *display = gtk_widget_get_display (popup->window);
    GdkSeat    *seat    = gdk_display_get_default_seat (display);
    gdk_seat_ungrab (seat);

    if (popup->press_handler) {
        g_signal_handler_disconnect (popup->window, popup->press_handler);
        popup->press_handler = 0;
    }

    gtk_widget_hide (popup->window);

    typedef void (*ClosedCb)(gpointer);
    ClosedCb cb   = g_object_get_data (G_OBJECT (popup->window), "closed-cb");
    gpointer data = g_object_get_data (G_OBJECT (popup->window), "closed-data");
    if (cb) cb (data);
}

void
popup_refresh (RemovablePopup *popup, GDBusConnection *conn)
{
    populate_content (popup, conn);
    position_popup (popup, popup->button);
}

void
popup_set_notify (RemovablePopup *popup, gboolean enabled)
{
    popup->show_notify = enabled;
}

void
popup_set_open_thunar (RemovablePopup *popup, gboolean enabled)
{
    popup->open_thunar = enabled;
}

void
popup_destroy (RemovablePopup *popup)
{
    if (!popup) return;
    gtk_widget_destroy (popup->window);
    if (popup->expanded)
        g_hash_table_destroy (popup->expanded);
    g_free (popup);
}
