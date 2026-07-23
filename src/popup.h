#ifndef POPUP_H
#define POPUP_H

#include <gtk/gtk.h>
#include <libxfce4panel/libxfce4panel.h>
#include "ud-dbus.h"

typedef struct {
    GtkWidget       *window;
    GtkWidget       *content_box;
    GtkWidget       *scroll;
    GtkWidget       *button;
    gulong           press_handler;
    gint             popup_width;
    gint             popup_height;
    gboolean         show_notify;
    gboolean         open_thunar;
    GHashTable      *expanded;    /* drive_path expandidos entre refrescos */
} RemovablePopup;

RemovablePopup *popup_create  (XfcePanelPlugin *plugin, GtkWidget *button);
void            popup_show    (RemovablePopup *popup, XfcePanelPlugin *plugin,
                               GtkWidget *button, GDBusConnection *conn,
                               gint popup_width, gint popup_height);
void            popup_hide    (RemovablePopup *popup);
void            popup_refresh        (RemovablePopup *popup, GDBusConnection *conn);
void            popup_set_notify     (RemovablePopup *popup, gboolean enabled);
void            popup_set_open_thunar(RemovablePopup *popup, gboolean enabled);
void            popup_destroy        (RemovablePopup *popup);

#endif /* POPUP_H */
