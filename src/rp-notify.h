#ifndef RP_NOTIFY_H
#define RP_NOTIFY_H

#include <glib.h>

/* Notificaciones por aparato: cada aparato (clave) tiene UNA burbuja, y cada
 * aviso nuevo de ese aparato reemplaza al anterior en vez de apilarse
 * (conectado → montado → listo para quitar…). Aparatos distintos sí se apilan.
 *
 * Claves (las mismas que usan las tablas de "ya vistos" de plugin.c):
 *   discos     → drive_path de UDisks2
 *   teléfonos  → UdPhone.key (devfile USB, o la URI)
 *   otros USB  → syspath
 * key == NULL: notificación suelta, no reemplaza ni se reemplaza. */
void     rp_notify                  (const gchar *key, const gchar *summary,
                                     const gchar *body, const gchar *icon);

/* Cierra todas las burbujas del plugin que estén en pantalla (al abrir el
 * popup: lo que decían ya se ve en la lista, y no quedan tapándolo). */
void     rp_notify_close_all        (void);

/* Después de expulsar o quitar con seguridad, el aparato desaparece enseguida
 * (UDisks2 / gvfs) y su "desconectado" reemplazaría a "Ya puede desenchufarlo"
 * con el aparato todavía enchufado. expect_removal marca la clave; la próxima
 * desconexión de ese aparato (dentro de unos segundos) no se notifica. */
void     rp_notify_expect_removal   (const gchar *key);
/* TRUE (y consume la marca) si esa desconexión era esperada */
gboolean rp_notify_removal_expected (const gchar *key);

#endif /* RP_NOTIFY_H */
