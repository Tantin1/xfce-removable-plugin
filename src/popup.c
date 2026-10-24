#include "popup.h"
#include "ud-dbus.h"
#include <gdk/gdk.h>
#ifdef GDK_WINDOWING_WAYLAND
#include <gdk/gdkwayland.h>
#endif
#include <libnotify/notify.h>
#include <string.h>
#include <sys/statvfs.h>

/* dominio propio: en Wayland todos los plugins comparten proceso y
   textdomain() pisaría el de los demás (wayland-notas §2.1, §3.2) */
#include <glib/gi18n-lib.h>

#define POPUP_WIDTH  400
#define POPUP_HEIGHT 300

/* diagnóstico silencioso: G_MESSAGES_DEBUG=xfce-removable-plugin xfce4-panel */
#define WL_DEBUG(...) g_log ("xfce-removable-plugin", G_LOG_LEVEL_DEBUG, __VA_ARGS__)

static gboolean
popup_is_wayland (GtkWidget *w)
{
#ifdef GDK_WINDOWING_WAYLAND
    return GDK_IS_WAYLAND_DISPLAY (gtk_widget_get_display (w));
#else
    (void) w;
    return FALSE;
#endif
}

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

/* Regla "imán" a lo largo del panel (wayland-notas §4.2b), un solo eje:
   1) centrar sobre el plugin, 2) correr hasta que entre,
   3) si contra un borde queda un hueco menor que medio popup, pegarlo ahí. */
static gint
place_along_panel (gint area_start, gint area_len,
                   gint btn_start,  gint btn_len, gint pop_len)
{
    gint area_end = area_start + area_len;
    gint p;

    if (pop_len >= area_len)
        return area_start;

    p = btn_start + btn_len / 2 - pop_len / 2;
    if (p < area_start)            p = area_start;
    if (p + pop_len > area_end)    p = area_end - pop_len;

    if (p - area_start < pop_len / 2)
        p = area_start;
    else if (area_end - (p + pop_len) < pop_len / 2)
        p = area_end - pop_len;
    return p;
}

static GtkArrowType
popup_arrow_type (RemovablePopup *popup)
{
    return popup->plugin ? xfce_panel_plugin_arrow_type (popup->plugin)
                         : GTK_ARROW_DOWN;
}

/* X11: coordenadas absolutas, gtk_window_move */
static void
position_popup_x11 (RemovablePopup *popup, GtkWidget *button)
{
    GdkMonitor     *monitor;
    GdkRectangle    wa;
    GtkRequisition  preferred;
    gint bx, by, bw, bh, pw, ph, x, y;

    gdk_window_get_origin (gtk_widget_get_window (button), &bx, &by);
    if (!gtk_widget_get_has_window (button)) {
        GtkAllocation a;
        gtk_widget_get_allocation (button, &a);
        bx += a.x;
        by += a.y;
    }
    bw = gtk_widget_get_allocated_width  (button);
    bh = gtk_widget_get_allocated_height (button);

    gtk_widget_get_preferred_size (popup->window, NULL, &preferred);
    pw = preferred.width;
    ph = preferred.height;

    monitor = gdk_display_get_monitor_at_point (
                  gtk_widget_get_display (button), bx + bw / 2, by + bh / 2);
    gdk_monitor_get_workarea (monitor, &wa);

    switch (popup_arrow_type (popup)) {
    case GTK_ARROW_LEFT:    /* panel a la derecha */
    case GTK_ARROW_RIGHT:   /* panel a la izquierda */
        y = place_along_panel (wa.y, wa.height, by, bh, ph);
        if (popup_arrow_type (popup) == GTK_ARROW_RIGHT) {
            x = bx + bw;
            if (x + pw > wa.x + wa.width) x = bx - pw;
        } else {
            x = bx - pw;
            if (x < wa.x) x = bx + bw;
        }
        break;
    case GTK_ARROW_UP:      /* panel abajo */
        x = place_along_panel (wa.x, wa.width, bx, bw, pw);
        y = by - ph;
        if (y < wa.y) y = by + bh;
        break;
    case GTK_ARROW_DOWN:    /* panel arriba */
    default:
        x = place_along_panel (wa.x, wa.width, bx, bw, pw);
        y = by + bh;
        if (y + ph > wa.y + wa.height) y = by - ph;
        break;
    }

    gtk_window_move (GTK_WINDOW (popup->window), x, y);
}

/* Wayland: el cliente no conoce su posición en pantalla. Se ancla a un
   rectángulo relativo a la ventana del panel con gdk_window_move_to_rect
   (wayland-notas §4.2 / §4.2b). Requiere la ventana realizada y sin mapear. */
static void
position_popup_wayland (RemovablePopup *popup, GtkWidget *button)
{
    GtkWidget      *toplevel = gtk_widget_get_toplevel (button);
    GtkRequisition  preferred;
    GdkRectangle    rect;
    GdkGravity      rect_anchor, win_anchor;
    GdkAnchorHints  hints;
    gint            bx = 0, by = 0, bw, bh, tw, th;

    gtk_widget_translate_coordinates (button, toplevel, 0, 0, &bx, &by);
    bw = MAX (gtk_widget_get_allocated_width  (button), 1);
    bh = MAX (gtk_widget_get_allocated_height (button), 1);
    tw = gtk_widget_get_allocated_width  (toplevel);
    th = gtk_widget_get_allocated_height (toplevel);

    gtk_widget_get_preferred_size (popup->window, NULL, &preferred);

    switch (popup_arrow_type (popup)) {
    case GTK_ARROW_RIGHT:   /* panel a la izquierda */
        rect = (GdkRectangle) { bx, place_along_panel (0, th, by, bh, preferred.height), bw, 1 };
        rect_anchor = GDK_GRAVITY_NORTH_EAST;
        win_anchor  = GDK_GRAVITY_NORTH_WEST;
        hints       = GDK_ANCHOR_FLIP_X | GDK_ANCHOR_SLIDE_Y;
        break;
    case GTK_ARROW_LEFT:    /* panel a la derecha */
        rect = (GdkRectangle) { bx, place_along_panel (0, th, by, bh, preferred.height), bw, 1 };
        rect_anchor = GDK_GRAVITY_NORTH_WEST;
        win_anchor  = GDK_GRAVITY_NORTH_EAST;
        hints       = GDK_ANCHOR_FLIP_X | GDK_ANCHOR_SLIDE_Y;
        break;
    case GTK_ARROW_UP:      /* panel abajo */
        rect = (GdkRectangle) { place_along_panel (0, tw, bx, bw, preferred.width), by, 1, bh };
        rect_anchor = GDK_GRAVITY_NORTH_WEST;
        win_anchor  = GDK_GRAVITY_SOUTH_WEST;
        hints       = GDK_ANCHOR_FLIP_Y | GDK_ANCHOR_SLIDE_X;
        break;
    case GTK_ARROW_DOWN:    /* panel arriba */
    default:
        rect = (GdkRectangle) { place_along_panel (0, tw, bx, bw, preferred.width), by, 1, bh };
        rect_anchor = GDK_GRAVITY_SOUTH_WEST;
        win_anchor  = GDK_GRAVITY_NORTH_WEST;
        hints       = GDK_ANCHOR_FLIP_Y | GDK_ANCHOR_SLIDE_X;
        break;
    }

    WL_DEBUG ("wayland: panel %dx%d, botón %d,%d %dx%d, popup %dx%d, rect %d,%d %dx%d",
              tw, th, bx, by, bw, bh, preferred.width, preferred.height,
              rect.x, rect.y, rect.width, rect.height);

    gdk_window_move_to_rect (gtk_widget_get_window (popup->window), &rect,
                             rect_anchor, win_anchor, hints, 0, 0);
}

static void
position_popup (RemovablePopup *popup, GtkWidget *button)
{
    if (popup->wayland)
        position_popup_wayland (popup, button);
    else
        position_popup_x11 (popup, button);
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

/* ---------- Wayland: clic afuera y cierre por el compositor ---------- */

/* Durante la captura, los clics en el panel llegan a la ventana del popup con
   coordenadas fuera de su área (wayland-notas §2.3): se decide por coordenadas. */
static gboolean
wl_on_button_press (GtkWidget *widget, GdkEventButton *ev, RemovablePopup *popup)
{
    (void) widget;
    GdkWindow *own    = gtk_widget_get_window (popup->window);
    GdkWindow *win    = ev->window;
    gdouble    x      = ev->x, y = ev->y;
    gboolean   inside = FALSE;

    while (win && win != own) {
        gdk_window_coords_to_parent (win, x, y, &x, &y);
        win = gdk_window_get_effective_parent (win);
    }
    if (win == own)
        inside = x >= 0 && y >= 0 &&
                 x < gtk_widget_get_allocated_width  (popup->window) &&
                 y < gtk_widget_get_allocated_height (popup->window);

    WL_DEBUG ("wayland: clic %s (%.0f,%.0f)", inside ? "adentro" : "afuera", x, y);

    if (inside)
        return GDK_EVENT_PROPAGATE;
    popup_hide (popup);
    return GDK_EVENT_STOP;   /* consumir: el botón del plugin no lo reabre */
}

/* Clic en otro programa: el compositor descarta el popup. En GTK 3.24 lo único
   que llega suele ser window-state-event (WITHDRAWN); se escuchan los tres
   (wayland-notas §4.5, corrección del bluetooth). */
static gboolean
wl_on_popup_gone (GtkWidget *widget, GdkEvent *ev, RemovablePopup *popup)
{
    if (popup->hiding || !gtk_widget_get_visible (widget))
        return FALSE;

    /* de window-state-event solo interesa el paso a WITHDRAWN (foco, etc. no) */
    if (ev && ev->type == GDK_WINDOW_STATE &&
        !(ev->window_state.new_window_state & GDK_WINDOW_STATE_WITHDRAWN))
        return FALSE;

    GdkWindow *gw = gtk_widget_get_window (widget);
    if (gw && gdk_window_is_visible (gw))
        return FALSE;   /* aviso tardío de un cierre anterior: la ventana ya se reabrió */

    WL_DEBUG ("wayland: el compositor cerró el popup (evento %d)", ev ? (gint) ev->type : -1);
    popup_hide (popup);
    return FALSE;
}

static void
wl_prepare_show (GdkSeat *seat, GdkWindow *window, gpointer data)
{
    (void) seat; (void) window;
    gtk_widget_show_all (GTK_WIDGET (data));
}

/* prototipo adelantado */
static void populate_content (RemovablePopup *popup, GDBusConnection *conn,
                              const UdScan *scan);
static void popup_rebuild    (RemovablePopup *popup, GDBusConnection *conn);

/* ---------- datos por fila de partición ---------- */

typedef struct {
    RemovablePopup  *popup;
    GDBusConnection *conn;
    gchar           *object_path;
    gchar           *drive_path;
    gchar           *mount_point;
    gboolean         mounted;
    GtkWidget       *btn_action;
} RowData;

static void
row_data_free (gpointer data)
{
    RowData *rd = data;
    g_free (rd->object_path);
    g_free (rd->drive_path);
    g_free (rd->mount_point);
    g_free (rd);
}

/* ---------- datos para el botón Eject ---------- */

typedef struct {
    RemovablePopup  *popup;
    GDBusConnection *conn;
    gchar           *drive_path;
    GSList          *partitions;
    gboolean         power_off;   /* PowerOff en vez de Eject */
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

/* ---------- abrir el gestor de archivos preferido ---------- */

/* acepta una ruta local (/run/media/…) o una URI (mtp://…, gphoto2://…) */
static void
open_file_manager (const gchar *path_or_uri)
{
    GError *err = NULL;
    gchar  *exo = g_find_program_in_path ("exo-open");
    gchar  *uri = g_path_is_absolute (path_or_uri)
                  ? g_filename_to_uri (path_or_uri, NULL, NULL)
                  : g_strdup (path_or_uri);

    if (exo) {
        /* respeta "Aplicaciones preferidas" de Xfce; Thunar abre mtp:// vía gvfs */
        gchar *argv[] = { exo, (gchar *) "--launch", (gchar *) "FileManager",
                          (gchar *) path_or_uri, NULL };
        g_spawn_async (NULL, argv, NULL, G_SPAWN_DEFAULT, NULL, NULL, NULL, &err);
        g_free (exo);
    } else if (uri) {
        /* el gestor de carpetas: mtp:// no tiene un manejador de esquema propio */
        GAppInfo *app = g_app_info_get_default_for_type ("inode/directory", FALSE);
        if (app) {
            GList one = { uri, NULL, NULL };
            g_app_info_launch_uris (app, &one, NULL, &err);
            g_object_unref (app);
        } else {
            g_app_info_launch_default_for_uri (uri, NULL, &err);
        }
    }

    if (err) {
        g_warning ("xfce-removable-plugin: no se pudo abrir %s: %s", path_or_uri, err->message);
        g_error_free (err);
    }
    g_free (uri);
}

/* ---------- operaciones asíncronas: ciclo de vida ----------
 *
 * Las respuestas DBus llegan después de que el usuario hizo click, y para
 * entonces un refresco (señal PropertiesChanged/InterfacesAdded) puede haber
 * destruido la fila y su RowData/EjectData. Por eso cada operación copia lo
 * que necesita, toma una referencia a la conexión, y apunta al botón con un
 * weak pointer (queda en NULL si el botón se destruye).
 *
 * pending_ops + destroyed evitan liberar el RemovablePopup mientras haya una
 * operación en vuelo (p. ej. se quita el plugin del panel durante un mount).
 */

static void
popup_op_begin (RemovablePopup *popup)
{
    popup->pending_ops++;
}

static void
popup_op_end (RemovablePopup *popup)
{
    popup->pending_ops--;
    if (popup->destroyed && popup->pending_ops == 0)
        g_free (popup);
}

/* traduce errores conocidos de UDisks2 a un mensaje legible */
static gchar *
describe_error (const GError *error, const gchar *fallback)
{
    if (!error)
        return g_strdup (fallback);

    gchar *remote = g_dbus_error_get_remote_error (error);
    gchar *msg;

    if (g_strcmp0 (remote, "org.freedesktop.UDisks2.Error.DeviceBusy") == 0) {
        msg = g_strdup (_("The device is in use by another application. "
                          "Close any open files or terminals on it and try again."));
    } else if (remote && g_str_has_prefix (remote, "org.freedesktop.UDisks2.Error.NotAuthorized")) {
        msg = g_strdup (_("Not authorized to perform this operation."));
    } else {
        GError *copy = g_error_copy (error);
        g_dbus_error_strip_remote_error (copy);
        msg = g_strdup_printf ("%s\n%s", fallback, copy->message);
        g_error_free (copy);
    }

    g_free (remote);
    return msg;
}

/* ---------- mount / unmount ---------- */

typedef struct {
    RemovablePopup  *popup;
    GDBusConnection *conn;
    gchar           *object_path;
    GtkWidget       *btn;          /* weak: NULL si la fila fue destruida */
} ActionOp;

static ActionOp *
action_op_new (RemovablePopup *popup, GDBusConnection *conn,
               const gchar *object_path, GtkWidget *btn)
{
    ActionOp *op    = g_new0 (ActionOp, 1);
    op->popup       = popup;
    op->conn        = g_object_ref (conn);
    op->object_path = g_strdup (object_path);
    op->btn         = btn;
    g_object_add_weak_pointer (G_OBJECT (btn), (gpointer *) &op->btn);
    popup_op_begin (popup);
    return op;
}

static void
action_op_free (ActionOp *op)
{
    if (op->btn)
        g_object_remove_weak_pointer (G_OBJECT (op->btn), (gpointer *) &op->btn);
    g_object_unref (op->conn);
    g_free (op->object_path);
    popup_op_end (op->popup);   /* puede liberar el popup: último uso */
    g_free (op);
}

static void
on_mount_done (gboolean ok, const gchar *mount_point,
               const GError *error, gpointer data)
{
    ActionOp       *op    = data;
    RemovablePopup *popup = op->popup;

    if (!popup->destroyed) {
        if (ok) {
            if (popup->show_notify)
                notify_send (_("Device mounted"), mount_point, "drive-removable-media");
            if (popup->open_thunar && mount_point) {
                /* igual que "Abrir carpeta": el popup no queda encima del gestor
                   (popup_show reconstruye al volver a abrir) */
                open_file_manager (mount_point);
                popup_hide (popup);
            } else if (op->btn) {
                /* si la señal de cambio todavía no reconstruyó la fila, forzarlo */
                popup_rebuild (popup, op->conn);
            }
        } else {
            if (popup->show_notify) {
                gchar *msg = describe_error (error, _("Could not mount the device."));
                notify_send (_("Mount failed"), msg, "dialog-error");
                g_free (msg);
            }
            if (op->btn) {
                gtk_button_set_label (GTK_BUTTON (op->btn), _("Mount"));
                gtk_widget_set_sensitive (op->btn, TRUE);
            }
        }
    }

    action_op_free (op);
}

static void
on_unmount_done (gboolean ok, const gchar *result,
                 const GError *error, gpointer data)
{
    (void) result;
    ActionOp       *op    = data;
    RemovablePopup *popup = op->popup;

    if (!popup->destroyed) {
        if (ok) {
            if (popup->show_notify)
                notify_send (_("Safe to remove"), _("You can now unplug the device."),
                             "drive-removable-media");
            if (op->btn)
                popup_rebuild (popup, op->conn);
        } else {
            if (popup->show_notify) {
                gchar *msg = describe_error (error, _("Could not unmount the device."));
                notify_send (_("Unmount failed"), msg, "dialog-error");
                g_free (msg);
            }
            if (op->btn) {
                gtk_button_set_label (GTK_BUTTON (op->btn), _("Unmount"));
                gtk_widget_set_sensitive (op->btn, TRUE);
            }
        }
    }

    action_op_free (op);
}

static void
on_mount_clicked (GtkWidget *btn, RowData *rd)
{
    gtk_widget_set_sensitive (btn, FALSE);
    gtk_button_set_label (GTK_BUTTON (btn), _("Mounting…"));
    ud_mount_async (rd->conn, rd->object_path, on_mount_done,
                    action_op_new (rd->popup, rd->conn, rd->object_path, btn));
}

static void
on_unmount_clicked (GtkWidget *btn, RowData *rd)
{
    gtk_widget_set_sensitive (btn, FALSE);
    gtk_button_set_label (GTK_BUTTON (btn), _("Unmounting…"));
    ud_unmount_async (rd->conn, rd->object_path, on_unmount_done,
                      action_op_new (rd->popup, rd->conn, rd->object_path, btn));
}

/* ---------- eject: desmonta en cadena y después expulsa ----------
 *
 * Si algún desmontaje falla se corta la cadena y NO se expulsa: decirle al
 * usuario "Safe to unplug" con una partición todavía montada puede costarle datos.
 */

typedef struct {
    RemovablePopup  *popup;
    GDBusConnection *conn;
    gchar           *drive_path;
    GSList          *pending;   /* object_paths que faltan desmontar */
    gboolean         power_off; /* PowerOff en vez de Eject */
    GtkWidget       *btn;       /* weak */
} EjectOp;

static void
eject_op_free (EjectOp *op)
{
    if (op->btn)
        g_object_remove_weak_pointer (G_OBJECT (op->btn), (gpointer *) &op->btn);
    g_object_unref (op->conn);
    g_free (op->drive_path);
    g_slist_free_full (op->pending, g_free);
    popup_op_end (op->popup);   /* puede liberar el popup: último uso */
    g_free (op);
}

static gpointer
copy_path (gconstpointer src, gpointer unused)
{
    (void) unused;
    return g_strdup (src);
}

static void eject_step (EjectOp *op);

static void
on_eject_unmount_done (gboolean ok, const gchar *result,
                       const GError *error, gpointer data)
{
    (void) result;
    EjectOp *op = data;

    if (op->popup->destroyed) {
        eject_op_free (op);
        return;
    }

    if (!ok) {
        if (op->popup->show_notify) {
            gchar *msg = describe_error (error, _("Could not unmount the device."));
            notify_send (_("Eject failed"), msg, "dialog-error");
            g_free (msg);
        }
        if (op->btn)
            gtk_widget_set_sensitive (op->btn, TRUE);
        /* alguna partición pudo haberse desmontado antes del fallo */
        popup_rebuild (op->popup, op->conn);
        eject_op_free (op);
        return;
    }

    eject_step (op);
}

static void
on_eject_done (gboolean ok, const gchar *result,
               const GError *error, gpointer data)
{
    (void) result;
    EjectOp *op = data;

    if (!op->popup->destroyed) {
        if (op->popup->show_notify) {
            if (ok) {
                notify_send (_("Device ejected"), _("Safe to unplug."), "drive-removable-media");
            } else {
                gchar *msg = describe_error (error,
                    _("The partitions were unmounted, but the drive could not be ejected."));
                notify_send (_("Eject failed"), msg, "dialog-error");
                g_free (msg);
            }
        }
        if (!ok && op->btn)
            gtk_widget_set_sensitive (op->btn, TRUE);
        popup_rebuild (op->popup, op->conn);
    }

    eject_op_free (op);
}

static void
eject_step (EjectOp *op)
{
    if (op->pending) {
        GSList *head = op->pending;
        op->pending  = g_slist_remove_link (op->pending, head);
        ud_unmount_async (op->conn, head->data, on_eject_unmount_done, op);
        g_free (head->data);   /* ud_call_async ya copió el path */
        g_slist_free_1 (head);
        return;
    }
    if (op->power_off)
        ud_power_off_async (op->conn, op->drive_path, on_eject_done, op);
    else
        ud_eject_async (op->conn, op->drive_path, on_eject_done, op);
}

static void
on_eject_clicked (GtkWidget *btn, EjectData *ed)
{
    gtk_widget_set_sensitive (btn, FALSE);

    if (ed->popup->show_notify)
        notify_send (_("Ejecting…"), _("Please wait, do not unplug."), "drive-removable-media");

    EjectOp *op    = g_new0 (EjectOp, 1);
    op->popup      = ed->popup;
    op->conn       = g_object_ref (ed->conn);
    op->drive_path = g_strdup (ed->drive_path);
    op->power_off  = ed->power_off;
    op->pending    = g_slist_copy_deep (ed->partitions, copy_path, NULL);
    op->btn        = btn;
    g_object_add_weak_pointer (G_OBJECT (btn), (gpointer *) &op->btn);
    popup_op_begin (op->popup);

    eject_step (op);
}

/* ---------- fila de partición (indentada) ---------- */

static void
on_open_folder_clicked (GtkWidget *btn, RowData *rd)
{
    (void) btn;
    if (rd->mount_point)
        open_file_manager (rd->mount_point);
    popup_hide (rd->popup);
}

/* "tamaño · tipo" si no está montada, "X free of Y · tipo" si lo está.
   fraction = fracción usada (0..1), o -1 si no se pudo calcular */
static gchar *
partition_detail (const UdDevice *dev, gdouble *fraction)
{
    gchar          *size_str;
    struct statvfs  st;

    *fraction = -1.0;
    if (dev->mounted && dev->mount_point &&
        statvfs (dev->mount_point, &st) == 0 && st.f_blocks > 0) {
        guint64 total = (guint64) st.f_blocks * st.f_frsize;
        guint64 avail = (guint64) st.f_bavail * st.f_frsize;
        guint64 freeb = (guint64) st.f_bfree  * st.f_frsize;
        gchar  *t     = format_size (total);
        gchar  *a     = format_size (avail);
        size_str  = g_strdup_printf (_("%s free of %s"), a, t);
        guint64 used  = total - freeb;
        /* misma fórmula que df: los bloques reservados para root no cuentan como libres */
        *fraction = (used + avail > 0) ? (gdouble) used / (gdouble) (used + avail) : 0.0;
        g_free (t);
        g_free (a);
    } else {
        size_str = format_size (dev->size);
    }

    gchar *detail = (dev->fs_type && *dev->fs_type)
                    ? g_strdup_printf ("%s · %s", size_str, dev->fs_type)
                    : g_strdup (size_str);
    g_free (size_str);
    return detail;
}

static GtkWidget *
build_partition_row (RemovablePopup *popup, GDBusConnection *conn, UdDevice *dev)
{
    RowData *rd     = g_new0 (RowData, 1);
    rd->popup       = popup;
    rd->conn        = conn;
    rd->object_path = g_strdup (dev->object_path);
    rd->drive_path  = g_strdup (dev->drive_path);
    rd->mount_point = g_strdup (dev->mount_point);
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

    gdouble  fraction;
    gchar   *detail = partition_detail (dev, &fraction);

    GtkWidget *detail_label = gtk_label_new (detail);
    g_free (detail);
    gtk_label_set_xalign (GTK_LABEL (detail_label), 0.0);
    gtk_label_set_ellipsize (GTK_LABEL (detail_label), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars (GTK_LABEL (detail_label), 24);
    gtk_style_context_add_class (gtk_widget_get_style_context (detail_label), "dim-label");

    gtk_box_pack_start (GTK_BOX (info_box), name_label,   FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (info_box), detail_label, FALSE, FALSE, 0);

    /* barra de uso: azul ("high") hasta 90%, naranja ("low") por encima */
    if (fraction >= 0.0) {
        GtkLevelBar *bar = GTK_LEVEL_BAR (gtk_level_bar_new_for_interval (0.0, 1.0));
        gtk_level_bar_remove_offset_value (bar, GTK_LEVEL_BAR_OFFSET_LOW);
        gtk_level_bar_remove_offset_value (bar, GTK_LEVEL_BAR_OFFSET_HIGH);
        gtk_level_bar_remove_offset_value (bar, GTK_LEVEL_BAR_OFFSET_FULL);
        gtk_level_bar_add_offset_value (bar, GTK_LEVEL_BAR_OFFSET_HIGH, 0.90);
        gtk_level_bar_add_offset_value (bar, GTK_LEVEL_BAR_OFFSET_LOW,  1.00);
        gtk_level_bar_set_value (bar, CLAMP (fraction, 0.0, 1.0));
        gtk_widget_set_margin_top (GTK_WIDGET (bar), 2);
        gtk_box_pack_start (GTK_BOX (info_box), GTK_WIDGET (bar), FALSE, FALSE, 0);
    }

    if (dev->mounted && dev->mount_point)
        gtk_widget_set_tooltip_text (info_box, dev->mount_point);

    gtk_box_pack_start (GTK_BOX (row), info_box, TRUE, TRUE, 0);

    if (dev->mounted && dev->mount_point) {
        GtkWidget *btn_open = gtk_button_new_from_icon_name (
            "folder-open-symbolic", GTK_ICON_SIZE_BUTTON);
        gtk_widget_set_tooltip_text (btn_open, _("Open folder"));
        gtk_widget_set_valign (btn_open, GTK_ALIGN_CENTER);
        g_signal_connect (btn_open, "clicked", G_CALLBACK (on_open_folder_clicked), rd);
        gtk_box_pack_start (GTK_BOX (row), btn_open, FALSE, FALSE, 0);
    }

    GtkWidget *btn_action;
    if (dev->mounted) {
        btn_action = gtk_button_new_with_label (_("Unmount"));
        g_signal_connect (btn_action, "clicked", G_CALLBACK (on_unmount_clicked), rd);
    } else {
        btn_action = gtk_button_new_with_label (_("Mount"));
        g_signal_connect (btn_action, "clicked", G_CALLBACK (on_mount_clicked), rd);
    }
    gtk_widget_set_valign (btn_action, GTK_ALIGN_CENTER);
    rd->btn_action = btn_action;
    gtk_box_pack_start (GTK_BOX (row), btn_action, FALSE, FALSE, 0);

    return row;
}

/* ---------- botón con ícono + texto fijo ----------
 *
 * Fila:  [flecha][ícono][ nombre ......... ][⏏ Expulsar]
 * - El texto del botón NUNCA se recorta.
 * - El nombre es el único elemento elástico: ancho mínimo chico
 *   (NAME_MIN_CHARS), se recorta con "…" y muestra el nombre entero en el tooltip.
 * - Si aun así la fila no entra en el ancho configurado del popup (popup muy
 *   angosto o traducción larga), fit_labeled_button() deja el botón solo con el
 *   ícono, con el texto en el tooltip, en vez de ensanchar el popup.
 * Ojo: GTK reparte el sobrante por igual entre los hijos que no llegan a su
 * ancho natural, así que dejar el texto del botón recortable lo recortaría
 * junto con el nombre. Por eso la decisión es explícita.
 */
#define NAME_MIN_CHARS 6

static GtkWidget *
labeled_button_new (const gchar *icon_name, const gchar *text)
{
    GtkWidget *btn   = gtk_button_new ();
    GtkWidget *box   = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
    GtkWidget *image = gtk_image_new_from_icon_name (icon_name, GTK_ICON_SIZE_BUTTON);
    GtkWidget *label = gtk_label_new (text);

    gtk_box_pack_start (GTK_BOX (box), image, FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (box), label, FALSE, FALSE, 0);
    gtk_container_add (GTK_CONTAINER (btn), box);
    g_object_set_data (G_OBJECT (btn), "btn-label", label);

    gtk_widget_set_tooltip_text (btn, text);
    gtk_widget_set_valign (btn, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand (btn, FALSE);
    return btn;
}

/* ancho útil del contenido: popup configurado - borde - barra de desplazamiento */
static gint
popup_content_width (RemovablePopup *popup)
{
    gint avail = popup->popup_width
               - 2 * (gint) gtk_container_get_border_width (GTK_CONTAINER (popup->window));
    GtkWidget *vbar = gtk_scrolled_window_get_vscrollbar (GTK_SCROLLED_WINDOW (popup->scroll));
    if (vbar && !gtk_scrolled_window_get_overlay_scrolling (GTK_SCROLLED_WINDOW (popup->scroll))) {
        gint bar_min = 0;
        gtk_widget_get_preferred_width (vbar, &bar_min, NULL);
        avail -= bar_min;
    }
    return avail;
}

/* llamar con la fila ya empacada y mostrada (un widget oculto no mide) */
static void
fit_labeled_button (RemovablePopup *popup, GtkWidget *row, GtkWidget *btn)
{
    GtkWidget *label = btn ? g_object_get_data (G_OBJECT (btn), "btn-label") : NULL;
    gint       row_min = 0;

    if (!label)
        return;
    gtk_widget_get_preferred_width (row, &row_min, NULL);
    if (row_min > popup_content_width (popup)) {
        gtk_widget_set_no_show_all (label, TRUE);
        gtk_widget_hide (label);
    }
}

/* nombre en negrita, elástico, recortable, con el texto completo en el tooltip */
static void
setup_elastic_name (GtkWidget *label, const gchar *full_name)
{
    gtk_label_set_xalign (GTK_LABEL (label), 0.0);
    gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
    gtk_label_set_width_chars     (GTK_LABEL (label), NAME_MIN_CHARS);
    gtk_label_set_max_width_chars (GTK_LABEL (label), -1);   /* natural = nombre entero */
    gtk_widget_set_hexpand (label, TRUE);
    if (full_name && *full_name)
        gtk_widget_set_tooltip_text (label, full_name);

    PangoAttrList *attrs = pango_attr_list_new ();
    pango_attr_list_insert (attrs, pango_attr_weight_new (PANGO_WEIGHT_BOLD));
    gtk_label_set_attributes (GTK_LABEL (label), attrs);
    pango_attr_list_unref (attrs);
}

/* ---------- fila de encabezado de disco ---------- */

/* nombre de respaldo si el Drive no tiene Vendor/Model:
   /dev/sdb1 -> /dev/sdb, /dev/mmcblk0p1 -> /dev/mmcblk0, /dev/nvme0n1p2 -> /dev/nvme0n1.
   Solo se recorta si el bloque es una partición (/dev/mmcblk0 sin tabla queda igual). */
static gchar *
fallback_drive_name (const UdDevice *dev)
{
    if (!dev->device || !*dev->device)
        return NULL;

    gchar *d = g_strdup (dev->device);
    if (dev->is_partition) {
        gsize len = strlen (d);
        while (len > 0 && g_ascii_isdigit (d[len - 1]))
            d[--len] = '\0';
        if (len > 1 && d[len - 1] == 'p' && g_ascii_isdigit (d[len - 2]))
            d[--len] = '\0';
    }
    return d;
}

static GtkWidget *
build_drive_header (RemovablePopup *popup, GDBusConnection *conn,
                    const gchar *drive_path, GSList *partitions,
                    GtkWidget *parts_box)
{
    (void) parts_box;

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

    /* nombre del disco: Vendor + Model (ya viene en el UdDevice, sin llamadas DBus) */
    UdDevice *first      = partitions ? partitions->data : NULL;
    gchar    *drive_name = NULL;
    if (first && first->drive_name && *first->drive_name)
        drive_name = g_strdup (first->drive_name);
    else if (first)
        drive_name = fallback_drive_name (first);
    if (!drive_name)
        drive_name = g_strdup (_("Removable disk"));

    GtkWidget *name_label = gtk_label_new (drive_name);
    setup_elastic_name (name_label, drive_name);
    g_free (drive_name);
    gtk_box_pack_start (GTK_BOX (row), name_label, TRUE, TRUE, 0);

    /* botón Eject / Safely remove */
    gboolean ejectable = FALSE, can_power_off = FALSE;
    GSList  *mounted_paths = NULL;
    for (GSList *l = partitions; l; l = l->next) {
        UdDevice *dev = l->data;
        if (dev->ejectable)     ejectable     = TRUE;
        if (dev->can_power_off) can_power_off = TRUE;
        if (dev->mounted)
            mounted_paths = g_slist_append (mounted_paths, g_strdup (dev->object_path));
    }

    /* Eject saca el medio (lector SD, óptico); PowerOff apaga el disco entero
       (HDD/SSD USB, que suelen reportar Ejectable=false) */
    if (ejectable || can_power_off) {
        EjectData *ejd  = g_new0 (EjectData, 1);
        ejd->popup      = popup;
        ejd->conn       = conn;
        ejd->drive_path = g_strdup (drive_path);
        ejd->partitions = mounted_paths;
        ejd->power_off  = !ejectable;

        GtkWidget *btn_eject = labeled_button_new (
            "media-eject-symbolic", ejectable ? _("Eject") : _("Safely remove"));
        g_object_set_data_full (G_OBJECT (btn_eject), "eject-data", ejd, eject_data_free);
        g_object_set_data (G_OBJECT (row), "labeled-btn", btn_eject);
        g_signal_connect (btn_eject, "clicked", G_CALLBACK (on_eject_clicked), ejd);
        gtk_box_pack_start (GTK_BOX (row), btn_eject, FALSE, FALSE, 0);
    } else {
        g_slist_free_full (mounted_paths, g_free);
    }

    return row;
}

/* ---------- fila de dispositivo USB no-almacenamiento ---------- */

/* sangría de "Other devices": sección secundaria, corrida a la derecha
   respecto de discos y teléfonos (filas: OTHER_INDENT + 32, como particiones) */
#define OTHER_INDENT 12

static GtkWidget *
build_usb_device_row (UdUsbDevice *dev)
{
    GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_start  (row, OTHER_INDENT + 32);
    gtk_widget_set_margin_end    (row, 12);
    gtk_widget_set_margin_top    (row, 4);
    gtk_widget_set_margin_bottom (row, 4);

    GtkWidget *icon = gtk_image_new_from_icon_name (
        ud_usb_type_icon (dev->type), GTK_ICON_SIZE_MENU);
    gtk_box_pack_start (GTK_BOX (row), icon, FALSE, FALSE, 0);

    GtkWidget *info_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_hexpand (info_box, TRUE);

    GtkWidget *name_label = gtk_label_new (dev->name);
    gtk_label_set_xalign (GTK_LABEL (name_label), 0.0);
    gtk_label_set_ellipsize (GTK_LABEL (name_label), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars (GTK_LABEL (name_label), 32);

    GtkWidget *type_label = gtk_label_new (_(ud_usb_type_label (dev->type)));
    gtk_label_set_xalign (GTK_LABEL (type_label), 0.0);
    gtk_style_context_add_class (gtk_widget_get_style_context (type_label), "dim-label");

    gtk_box_pack_start (GTK_BOX (info_box), name_label, FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (info_box), type_label, FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (row), info_box, TRUE, TRUE, 0);

    return row;
}

/* ---------- teléfonos y cámaras (gvfs) ----------
 *
 * Mismo ciclo de vida que ActionOp: la fila puede destruirse (un refresco por
 * mount-added/removed) antes de que vuelva la respuesta, así que la operación
 * guarda sus propias referencias y un weak pointer al botón. */

typedef struct {
    RemovablePopup  *popup;
    GDBusConnection *conn;
    GVolume         *volume;   /* ref o NULL */
    GMount          *mount;    /* ref o NULL */
    gchar           *uri;
} PhoneRowData;

static void
phone_row_data_free (gpointer data)
{
    PhoneRowData *pd = data;
    if (pd->volume) g_object_unref (pd->volume);
    if (pd->mount)  g_object_unref (pd->mount);
    g_free (pd->uri);
    g_free (pd);
}

typedef struct {
    RemovablePopup  *popup;
    GDBusConnection *conn;
    GtkWidget       *btn;          /* weak */
    gchar           *restore;      /* etiqueta a reponer si falla (NULL: botón de ícono) */
    gboolean         eject;        /* g_mount_eject en vez de g_mount_unmount */
} PhoneOp;

static PhoneOp *
phone_op_new (PhoneRowData *pd, GtkWidget *btn, const gchar *restore)
{
    PhoneOp *op = g_new0 (PhoneOp, 1);
    op->popup   = pd->popup;
    op->conn    = pd->conn ? g_object_ref (pd->conn) : NULL;
    op->btn     = btn;
    op->restore = g_strdup (restore);
    g_object_add_weak_pointer (G_OBJECT (btn), (gpointer *) &op->btn);
    popup_op_begin (op->popup);
    return op;
}

static void
phone_op_free (PhoneOp *op)
{
    if (op->btn)
        g_object_remove_weak_pointer (G_OBJECT (op->btn), (gpointer *) &op->btn);
    if (op->conn)
        g_object_unref (op->conn);
    g_free (op->restore);
    popup_op_end (op->popup);   /* puede liberar el popup: último uso */
    g_free (op);
}

static void
phone_op_failed (PhoneOp *op, const GError *err, const gchar *summary,
                 const gchar *fallback)
{
    /* FAILED_HANDLED: el usuario canceló un diálogo de gvfs, no es un error */
    if (op->popup->show_notify &&
        !g_error_matches (err, G_IO_ERROR, G_IO_ERROR_FAILED_HANDLED)) {
        gchar *msg = describe_error (err, fallback);
        notify_send (summary, msg, "dialog-error");
        g_free (msg);
    }
    if (op->btn) {
        if (op->restore)
            gtk_button_set_label (GTK_BUTTON (op->btn), op->restore);
        gtk_widget_set_sensitive (op->btn, TRUE);
    }
}

static void
on_phone_mount_done (GObject *source, GAsyncResult *res, gpointer data)
{
    PhoneOp *op  = data;
    GError  *err = NULL;
    gboolean ok  = g_volume_mount_finish (G_VOLUME (source), res, &err);

    if (!op->popup->destroyed) {
        if (ok) {
            /* Con MTP, al terminar el montaje el GMount puede no estar registrado
               todavía en el monitor de gvfs (llega después con mount-added).
               Respaldo: la activation root del volumen, la misma mtp://… */
            GMount *mount = g_volume_get_mount (G_VOLUME (source));
            GFile  *root  = mount ? g_mount_get_root (mount)
                                  : g_volume_get_activation_root (G_VOLUME (source));
            gchar  *uri   = root ? g_file_get_uri (root) : NULL;
            gchar  *name  = g_volume_get_name (G_VOLUME (source));

            if (!uri)
                g_warning ("xfce-removable-plugin: teléfono montado sin URI para abrir");

            if (op->popup->show_notify)
                notify_send (_("Phone ready"), name, "phone");
            if (op->popup->open_thunar && uri) {
                open_file_manager (uri);
                popup_hide (op->popup);
            } else if (op->btn) {
                /* sin bus del sistema conn es NULL: popup_rebuild lo tolera */
                popup_rebuild (op->popup, op->conn);
            }

            g_free (name);
            g_free (uri);
            if (root)  g_object_unref (root);
            if (mount) g_object_unref (mount);
        } else {
            g_warning ("xfce-removable-plugin: montar teléfono: %s", err->message);
            phone_op_failed (op, err, _("Mount failed"),
                             _("Could not open the device. If it is a phone, unlock it "
                               "and choose \"File transfer\" in its USB notification."));
        }
    }

    g_clear_error (&err);
    phone_op_free (op);
}

static void
on_phone_unmount_done (GObject *source, GAsyncResult *res, gpointer data)
{
    PhoneOp *op  = data;
    GError  *err = NULL;
    gboolean ok  = op->eject
                   ? g_mount_eject_with_operation_finish   (G_MOUNT (source), res, &err)
                   : g_mount_unmount_with_operation_finish (G_MOUNT (source), res, &err);

    if (!op->popup->destroyed) {
        if (ok) {
            if (op->popup->show_notify)
                notify_send (_("Safe to remove"), _("You can now unplug the device."), "phone");
            popup_rebuild (op->popup, op->conn);
        } else {
            g_warning ("xfce-removable-plugin: desmontar teléfono: %s", err->message);
            phone_op_failed (op, err, _("Unmount failed"),
                             _("Could not close the connection to the device."));
        }
    }

    g_clear_error (&err);
    phone_op_free (op);
}

static void
on_phone_mount_clicked (GtkWidget *btn, PhoneRowData *pd)
{
    if (!pd->volume)
        return;
    gtk_widget_set_sensitive (btn, FALSE);
    gtk_button_set_label (GTK_BUTTON (btn), _("Mounting…"));
    /* sin GMountOperation: MTP/PTP no piden contraseña, y un diálogo con el
       popup capturando el teclado no la recibiría (wayland-notas §4.7) */
    g_volume_mount (pd->volume, G_MOUNT_MOUNT_NONE, NULL, NULL,
                    on_phone_mount_done, phone_op_new (pd, btn, _("Mount")));
}

static void
on_phone_unmount_clicked (GtkWidget *btn, PhoneRowData *pd)
{
    if (!pd->mount)
        return;
    gtk_widget_set_sensitive (btn, FALSE);

    PhoneOp *op = phone_op_new (pd, btn, NULL);
    op->eject   = g_mount_can_eject (pd->mount);
    if (op->eject) {
        g_mount_eject_with_operation (pd->mount, G_MOUNT_UNMOUNT_NONE, NULL, NULL,
                                      on_phone_unmount_done, op);
    } else {
        g_mount_unmount_with_operation (pd->mount, G_MOUNT_UNMOUNT_NONE, NULL, NULL,
                                        on_phone_unmount_done, op);
    }
}

static void
on_phone_open_clicked (GtkWidget *btn, PhoneRowData *pd)
{
    (void) btn;
    if (pd->uri)
        open_file_manager (pd->uri);
    popup_hide (pd->popup);
}

static const gchar *
phone_kind_label (UdPhoneKind kind)
{
    switch (kind) {
    case UD_PHONE_PTP: return _("Camera (PTP)");
    case UD_PHONE_AFC: return _("iPhone / iPad");
    case UD_PHONE_MTP:
    default:           return _("Phone (MTP)");
    }
}

static GtkWidget *
build_phone_row (RemovablePopup *popup, GDBusConnection *conn, UdPhone *ph)
{
    PhoneRowData *pd = g_new0 (PhoneRowData, 1);
    pd->popup  = popup;
    pd->conn   = conn;
    pd->volume = ph->volume ? g_object_ref (ph->volume) : NULL;
    pd->mount  = ph->mount  ? g_object_ref (ph->mount)  : NULL;
    pd->uri    = g_strdup (ph->uri);

    /* mismos márgenes que el encabezado de disco: es un aparato, no una partición */
    GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_start  (row, 12);
    gtk_widget_set_margin_end    (row, 12);
    gtk_widget_set_margin_top    (row, 8);
    gtk_widget_set_margin_bottom (row, 8);
    g_object_set_data_full (G_OBJECT (row), "phone-data", pd, phone_row_data_free);

    /* hueco del ancho de la flecha, para alinear con los discos */
    GtkWidget *spacer = gtk_image_new_from_icon_name ("pan-end-symbolic", GTK_ICON_SIZE_MENU);
    gtk_widget_set_opacity (spacer, 0.0);
    gtk_box_pack_start (GTK_BOX (row), spacer, FALSE, FALSE, 0);

    GtkWidget *icon = ph->icon
        ? gtk_image_new_from_gicon (ph->icon, GTK_ICON_SIZE_LARGE_TOOLBAR)
        : gtk_image_new_from_icon_name (ph->kind == UD_PHONE_PTP ? "camera-photo-symbolic"
                                                                 : "phone-symbolic",
                                        GTK_ICON_SIZE_LARGE_TOOLBAR);
    gtk_box_pack_start (GTK_BOX (row), icon, FALSE, FALSE, 0);

    GtkWidget *info_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_hexpand (info_box, TRUE);
    gtk_widget_set_valign (info_box, GTK_ALIGN_CENTER);

    const gchar *ph_name   = (ph->name && *ph->name) ? ph->name : _("Phone");
    GtkWidget   *name_label = gtk_label_new (ph_name);
    setup_elastic_name (name_label, ph_name);

    gchar *detail = ph->mount
        ? g_strdup (phone_kind_label (ph->kind))
        : g_strdup_printf ("%s · %s", phone_kind_label (ph->kind), _("not mounted"));
    GtkWidget *detail_label = gtk_label_new (detail);
    g_free (detail);
    gtk_label_set_xalign (GTK_LABEL (detail_label), 0.0);
    gtk_label_set_ellipsize (GTK_LABEL (detail_label), PANGO_ELLIPSIZE_END);
    gtk_style_context_add_class (gtk_widget_get_style_context (detail_label), "dim-label");

    gtk_box_pack_start (GTK_BOX (info_box), name_label,   FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (info_box), detail_label, FALSE, FALSE, 0);
    if (ph->uri)
        gtk_widget_set_tooltip_text (info_box, ph->uri);
    gtk_box_pack_start (GTK_BOX (row), info_box, TRUE, TRUE, 0);

    if (ph->mount) {
        GtkWidget *btn_open = gtk_button_new_from_icon_name (
            "folder-open-symbolic", GTK_ICON_SIZE_BUTTON);
        gtk_widget_set_tooltip_text (btn_open, _("Open folder"));
        gtk_widget_set_valign (btn_open, GTK_ALIGN_CENTER);
        g_signal_connect (btn_open, "clicked", G_CALLBACK (on_phone_open_clicked), pd);
        gtk_box_pack_start (GTK_BOX (row), btn_open, FALSE, FALSE, 0);

        if (g_mount_can_unmount (ph->mount) || g_mount_can_eject (ph->mount)) {
            GtkWidget *btn_off = labeled_button_new ("media-eject-symbolic",
                                                     _("Safely remove"));
            g_signal_connect (btn_off, "clicked", G_CALLBACK (on_phone_unmount_clicked), pd);
            g_object_set_data (G_OBJECT (row), "labeled-btn", btn_off);
            gtk_box_pack_start (GTK_BOX (row), btn_off, FALSE, FALSE, 0);
        }
    } else if (ph->volume && g_volume_can_mount (ph->volume)) {
        GtkWidget *btn_mount = gtk_button_new_with_label (_("Mount"));
        gtk_widget_set_valign (btn_mount, GTK_ALIGN_CENTER);
        g_signal_connect (btn_mount, "clicked", G_CALLBACK (on_phone_mount_clicked), pd);
        gtk_box_pack_start (GTK_BOX (row), btn_mount, FALSE, FALSE, 0);
    }

    return row;
}

/* ---------- poblar contenido ---------- */

static void
add_separator (RemovablePopup *popup)
{
    GtkWidget *sep = gtk_separator_new (GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start (GTK_BOX (popup->content_box), sep, FALSE, FALSE, 0);
    gtk_widget_show (sep);
}

/* sección "Other devices" con expand. Orden del patrón: ver CONTEXT.md
   ("Patrón expand/collapse — NO TOCAR sin leer esto") */
static void
add_other_devices_section (RemovablePopup *popup, GSList *usb_devs)
{
    /* usb_box: contenedor de filas, oculto por defecto */
    GtkWidget *usb_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    for (GSList *l = usb_devs; l; l = l->next) {
        GtkWidget *row = build_usb_device_row (l->data);
        gtk_box_pack_start (GTK_BOX (usb_box), row, FALSE, FALSE, 0);
    }
    /* encabezado "Other devices" con flecha */
    GtkWidget *hdr = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_start  (hdr, OTHER_INDENT + 12);
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

    /* ícono chico y título atenuado sin negrita: se lee como accesorio */
    GtkWidget *hdr_icon = gtk_image_new_from_icon_name (
        "emblem-system-symbolic", GTK_ICON_SIZE_MENU);
    GtkWidget *hdr_label = gtk_label_new (_("Other devices"));
    gtk_label_set_xalign (GTK_LABEL (hdr_label), 0.0);
    gtk_widget_set_hexpand (hdr_label, TRUE);
    gtk_style_context_add_class (gtk_widget_get_style_context (hdr_label), "dim-label");
    gtk_style_context_add_class (gtk_widget_get_style_context (hdr_icon),  "dim-label");

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

/* discos agrupados por drive_path; devuelve TRUE si agregó alguno */
static gboolean
add_storage_section (RemovablePopup *popup, GDBusConnection *conn, GSList *devices)
{
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

        if (!first_drive)
            add_separator (popup);
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
        fit_labeled_button (popup, event_box,
                            g_object_get_data (G_OBJECT (header), "labeled-btn"));

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
    return !first_drive;
}

/* orden: discos · teléfonos y cámaras · otros dispositivos */
static void
populate_content (RemovablePopup *popup, GDBusConnection *conn, const UdScan *scan)
{
    GList *kids = gtk_container_get_children (GTK_CONTAINER (popup->content_box));
    for (GList *k = kids; k; k = k->next)
        gtk_widget_destroy (k->data);
    g_list_free (kids);

    if (ud_scan_is_empty (scan)) {
        GtkWidget *label = gtk_label_new (_("No removable devices"));
        gtk_widget_set_margin_top    (label, 16);
        gtk_widget_set_margin_bottom (label, 16);
        gtk_widget_set_margin_start  (label, 16);
        gtk_widget_set_margin_end    (label, 16);
        gtk_box_pack_start (GTK_BOX (popup->content_box), label, TRUE, TRUE, 0);
        gtk_widget_show (label);
        return;
    }

    gboolean need_sep = add_storage_section (popup, conn, scan->storage);

    for (GSList *l = scan->phones; l; l = l->next) {
        if (need_sep)
            add_separator (popup);
        GtkWidget *row = build_phone_row (popup, conn, l->data);
        gtk_box_pack_start (GTK_BOX (popup->content_box), row, FALSE, FALSE, 0);
        gtk_widget_show_all (row);
        fit_labeled_button (popup, row, g_object_get_data (G_OBJECT (row), "labeled-btn"));
        need_sep = TRUE;
    }

    if (scan->usb) {
        if (need_sep)
            add_separator (popup);
        add_other_devices_section (popup, scan->usb);
    }
}

/* escanea y reconstruye (para cuando no hay listas a mano) */
static void
popup_rebuild (RemovablePopup *popup, GDBusConnection *conn)
{
    UdScan *scan = ud_scan (conn);
    populate_content (popup, conn, scan);
    ud_scan_free (scan);
}

/* ---------- crear popup ---------- */

RemovablePopup *
popup_create (XfcePanelPlugin *plugin, GtkWidget *button)
{
    RemovablePopup *popup = g_new0 (RemovablePopup, 1);

    GtkWidget *win = gtk_window_new (GTK_WINDOW_TOPLEVEL);
    popup->plugin  = plugin;
    popup->wayland = popup_is_wayland (win);
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

    if (popup->wayland) {
        /* una sola vez: avisos de que el compositor descartó el popup */
        g_signal_connect (win, "unmap-event",        G_CALLBACK (wl_on_popup_gone), popup);
        g_signal_connect (win, "grab-broken-event",  G_CALLBACK (wl_on_popup_gone), popup);
        g_signal_connect (win, "window-state-event", G_CALLBACK (wl_on_popup_gone), popup);
    }

    return popup;
}

/* ---------- mostrar popup ---------- */

/* Wayland (wayland-notas §4.2–4.4): popup hijo del panel, ubicado con
   move_to_rect y mostrado DENTRO de la captura para recibir teclado. */
static void
popup_show_wayland (RemovablePopup *popup, GtkWidget *button)
{
    GtkWidget *win      = popup->window;
    GtkWidget *toplevel = gtk_widget_get_toplevel (button);   /* XfcePanelWindow */

    if (GTK_IS_WINDOW (toplevel)) {
        if (gtk_window_get_transient_for (GTK_WINDOW (win)) != GTK_WINDOW (toplevel))
            gtk_window_set_transient_for (GTK_WINDOW (win), GTK_WINDOW (toplevel));
        WL_DEBUG ("wayland: ventana del panel = %s", G_OBJECT_TYPE_NAME (toplevel));
    } else {
        WL_DEBUG ("wayland: el botón no tiene ventana de nivel superior");
    }

    gtk_widget_realize (win);
    position_popup_wayland (popup, button);

    GdkSeat      *seat = gdk_display_get_default_seat (gtk_widget_get_display (win));
    GdkGrabStatus st   = gdk_seat_grab (seat, gtk_widget_get_window (win),
                                        GDK_SEAT_CAPABILITY_ALL, TRUE, NULL, NULL,
                                        wl_prepare_show, win);

    if (st == GDK_GRAB_SUCCESS) {
        /* mismo grupo que el panel, para que la captura de GTK abarque sus clics */
        if (GTK_IS_WINDOW (toplevel))
            gtk_window_group_add_window (gtk_window_get_group (GTK_WINDOW (toplevel)),
                                         GTK_WINDOW (win));
        gtk_grab_add (win);
        popup->press_handler = g_signal_connect (win, "button-press-event",
                                                 G_CALLBACK (wl_on_button_press), popup);
        WL_DEBUG ("wayland: captura OK, captura GTK %s",
                  gtk_widget_has_grab (win) ? "activa" : "NO activa");
    } else {
        WL_DEBUG ("wayland: captura FALLÓ (%d), cierre por focus-out", (gint) st);
        if (!gtk_widget_get_visible (win))
            gtk_widget_show_all (win);
        popup->press_handler = g_signal_connect (win, "focus-out-event",
                                                 G_CALLBACK (on_focus_out), popup);
    }
}

void
popup_show (RemovablePopup *popup, XfcePanelPlugin *plugin,
            GtkWidget *button, GDBusConnection *conn,
            gint popup_width, gint popup_height)
{
    if (plugin)
        popup->plugin = plugin;

    /* aplicar tamaño configurado (bug: antes se ignoraba) */
    popup->popup_width  = (popup_width  > 0) ? popup_width  : POPUP_WIDTH;
    popup->popup_height = (popup_height > 0) ? popup_height : POPUP_HEIGHT;

    g_object_set_data (G_OBJECT (popup->window), "conn", conn);

    popup_rebuild (popup, conn);

    gtk_widget_set_size_request (popup->window, popup->popup_width, -1);
    gtk_widget_set_size_request (popup->scroll, popup->popup_width, popup->popup_height);

    if (popup->wayland) {
        popup_show_wayland (popup, button);
        return;
    }

    /* ---- X11 (sin cambios de comportamiento) ---- */

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
        WL_DEBUG ("x11: grab OK, usando button-press");
        popup->press_handler = g_signal_connect (popup->window, "button-press-event",
                                                 G_CALLBACK (on_button_press), popup);
    } else {
        WL_DEBUG ("x11: grab FALLÓ, usando focus-out");
        popup->press_handler = g_signal_connect (popup->window, "focus-out-event",
                                                 G_CALLBACK (on_focus_out), popup);
    }
}

/* ---------- ocultar / destruir ---------- */

void
popup_hide (RemovablePopup *popup)
{
    if (!popup->window || popup->hiding)
        return;
    popup->hiding = TRUE;   /* soltar la captura puede disparar wl_on_popup_gone */

    GdkDisplay *display = gtk_widget_get_display (popup->window);
    GdkSeat    *seat    = gdk_display_get_default_seat (display);
    gdk_seat_ungrab (seat);
    if (gtk_widget_has_grab (popup->window))
        gtk_grab_remove (popup->window);

    if (popup->press_handler) {
        g_signal_handler_disconnect (popup->window, popup->press_handler);
        popup->press_handler = 0;
    }

    gtk_widget_hide (popup->window);

    /* Wayland: el botón no recibió "el mouse se fue" durante la captura (§4.8) */
    if (popup->wayland && popup->button) {
        gtk_widget_unset_state_flags (popup->button,
                                      GTK_STATE_FLAG_PRELIGHT | GTK_STATE_FLAG_ACTIVE);
        GtkWidget *top = gtk_widget_get_toplevel (popup->button);
        if (GTK_IS_WINDOW (top))
            gtk_window_set_focus_visible (GTK_WINDOW (top), FALSE);
    }

    popup->hiding = FALSE;

    typedef void (*ClosedCb)(gpointer);
    ClosedCb cb   = g_object_get_data (G_OBJECT (popup->window), "closed-cb");
    gpointer data = g_object_get_data (G_OBJECT (popup->window), "closed-data");
    if (cb) cb (data);
}

void
popup_refresh (RemovablePopup *popup, GDBusConnection *conn,
               const UdScan *scan)
{
    populate_content (popup, conn, scan);
    /* En Wayland un xdg_popup ya mapeado no se reubica; el tamaño es fijo
       (scroll con size_request), así que no hace falta. */
    if (!popup->wayland)
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
    /* que la destrucción (unmap, grab-broken…) no vuelva a entrar en popup_hide */
    g_signal_handlers_disconnect_by_data (popup->window, popup);
    popup->press_handler = 0;
    if (gtk_widget_get_visible (popup->window)) {
        gdk_seat_ungrab (gdk_display_get_default_seat (gtk_widget_get_display (popup->window)));
        if (gtk_widget_has_grab (popup->window))
            gtk_grab_remove (popup->window);
    }
    gtk_widget_destroy (popup->window);
    popup->window      = NULL;
    popup->content_box = NULL;
    if (popup->expanded) {
        g_hash_table_destroy (popup->expanded);
        popup->expanded = NULL;
    }
    /* si hay mount/unmount/eject en vuelo, el último callback libera el struct */
    popup->destroyed = TRUE;
    if (popup->pending_ops == 0)
        g_free (popup);
}
