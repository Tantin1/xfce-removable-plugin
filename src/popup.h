#ifndef POPUP_H
#define POPUP_H

#include <gtk/gtk.h>
#include <libxfce4panel/libxfce4panel.h>
#include "ud-dbus.h"

/* Estilo de la ventana (Propiedades → Apariencia). El valor se guarda como entero. */
enum {
    POPUP_STYLE_AUTO     = 0,   /* flotante si el tema la redondea y hay compositor */
    POPUP_STYLE_FLOATING = 1,   /* separada del panel siempre */
    POPUP_STYLE_ATTACHED = 2,   /* pegada al panel, opaca y recta (el aspecto de antes) */
};

typedef struct {
    XfcePanelPlugin *plugin;      /* para arrow_type (lado del panel) */
    GtkWidget       *window;
    GtkWidget       *frame;       /* caja con fondo, borde y esquinas del tema (nodo CSS "popover") */
    gboolean         rounded;     /* ventana con transparencia: el tema puede redondear las esquinas */
    gint             style;       /* POPUP_STYLE_* */
    gint             gap;         /* separación flotante de esta apertura (0 o POPUP_FLOAT_GAP) */
    gint             placed_w, placed_h; /* X11: tamaño con el que se ubicó (ver on_popup_size_allocate) */
    GtkWidget       *content_box;
    GtkWidget       *scroll;
    GtkWidget       *button;
    gulong           press_handler;
    gint             popup_width;
    gint             popup_height;
    gboolean         show_notify;
    gboolean         first_run_hint; /* explicar que el botón se oculta (modo primera vez) */
    GHashTable      *expanded;    /* claves (drive_path, "__other__") que el usuario
                                     cambió respecto de su estado por defecto */
    gint             pending_ops; /* mount/unmount/eject asíncronos en vuelo */
    gboolean         destroyed;   /* popup_destroy llamado con ops pendientes */
    gboolean         hiding;      /* dentro de popup_hide: cortar re-entradas */
    gboolean         wayland;     /* sesión Wayland (se decide al crear) */

    /* resultado de la última operación, mostrado arriba de la lista mientras
       el popup está abierto (en vez de una notificación que lo taparía).
       Se guarda aparte porque cada refresco rearma la lista. */
    gchar           *status_title;
    gchar           *status_text;
    gboolean         status_error;
    GtkWidget       *status_row;  /* weak: la línea en content_box, o NULL */
} RemovablePopup;

RemovablePopup *popup_create  (XfcePanelPlugin *plugin, GtkWidget *button);
/* scan: la última revisión de plugin.c (ya filtrada); NULL = revisar ahora */
void            popup_show    (RemovablePopup *popup, XfcePanelPlugin *plugin,
                               GtkWidget *button, GDBusConnection *conn,
                               gint popup_width, gint popup_height,
                               const UdScan *scan);
void            popup_hide    (RemovablePopup *popup);
void            popup_refresh        (RemovablePopup *popup, GDBusConnection *conn,
                                      const UdScan *scan);
void            popup_set_notify     (RemovablePopup *popup, gboolean enabled);
void            popup_set_first_run_hint (RemovablePopup *popup, gboolean enabled);
void            popup_set_style      (RemovablePopup *popup, gint style);
void            popup_destroy        (RemovablePopup *popup);

#endif /* POPUP_H */
