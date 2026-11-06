# Popup con los colores de los menús del tema

Receta para llevar a **xfce-net-plugin** y **xfce-bluetooth-plugin** lo que en
xfce-removable-plugin resolvieron los parches 23 y 24 (confirmado por el usuario en su
panel). Leer completo antes de tocar el popup de otro plugin.

Complementa a `popup-moderno-notas.md` (popup flotante, imán, transparencia). Esto
cambia **de qué elemento del tema salen los colores**.

---

## El problema

Con Greybird, y sobre todo con **Greybird-dark**, el popup de nuestros plugins se ve
gris claro y cuesta leerlo. Los plugins nativos de Xfce (pulseaudio, notifyd, power
manager) se ven bien con el mismo tema.

## La causa (medida, no supuesta)

Nuestros popups usaban una caja con nodo CSS **`popover`** y clase `background`, para
tomar fondo, borde y esquinas de los popovers del tema. Los plugins nativos son
**`GtkMenu`**: toman los colores del nodo **`menu`**. En la mayoría de los temas
coinciden; en Greybird-dark no.

Colores leídos con `gtk_style_context_get` (Greybird 3.23.3):

| Tema | `popover.background` | `menu` |
|---|---|---|
| Greybird-dark | `#676d6e`, contraste 4,5 con el texto | `#303133`, contraste 11,2 |
| Greybird | `#fcfcfc` | `#fcfcfc` |
| Adwaita | `#f6f5f4` | `#ffffff` |
| Adwaita oscuro | casi igual | `#2f2f2f` |

El texto secundario (`dim-label`) es semitransparente, así que sobre el gris de
Greybird-dark queda todavía peor que el 4,5 de la tabla.

## La regla (lo que quiere el usuario)

- **Los colores son SIEMPRE los del menú del tema, en los tres estilos de ventana**
  (Automático, Flotante y Pegada al panel).
- El estilo solo cambia la **separación** del panel y las **esquinas**:
  - Pegada → recta, opaca, sin separación.
  - Flotante con compositor → transparente alrededor, con las esquinas redondeadas
    según el radio que el tema da a sus **popovers** (los menús casi siempre tienen 0).
  - Flotante sin compositor → separada pero recta.
- Automático no cambia: decide flotante/pegada midiendo el radio de los **popovers**
  (Greybird da 5 < 6 → pegada, con puntas rectas; es lo correcto).

**Error que no hay que repetir:** el parche 23 usaba el menú solo en modo pegado y
dejaba el popover en flotante. El usuario lo rechazó: con Flotante manual en
Greybird-dark volvía el gris. Por eso el 24 usa el menú en todos los estilos.

---

## Implementación (así quedó en removable, `popup.c`)

### 1. Marco visible con nodo `menu`

El nombre de nodo CSS es por clase, así que hace falta una subclase mínima de
`GtkBox` (además de la de nodo `popover` que ya existía, ver punto 3):

```c
typedef struct { GtkBox parent; } RpMenuFrame;
typedef struct { GtkBoxClass parent_class; } RpMenuFrameClass;
G_DEFINE_TYPE (RpMenuFrame, rp_menu_frame, GTK_TYPE_BOX)

static void
rp_menu_frame_class_init (RpMenuFrameClass *klass)
{
    gtk_widget_class_set_css_name (GTK_WIDGET_CLASS (klass), "menu");
}

static void
rp_menu_frame_init (RpMenuFrame *self) { (void) self; }
```

Al crear el popup, ese es el marco que va dentro de la ventana, con la lista adentro:

```c
GtkWidget *frame = g_object_new (rp_menu_frame_get_type (),
                                 "orientation", GTK_ORIENTATION_VERTICAL, NULL);
gtk_style_context_add_class (gtk_widget_get_style_context (frame), "rp-frame");
/* SIN "background": ver trampas */
gtk_container_add (GTK_CONTAINER (win), frame);
```

### 2. CSS propio

Reemplaza las reglas `popover.rp-frame …` (prioridad APPLICATION):

```css
window.rp-popup.rp-rounded { background-color: transparent; background-image: none; }
menu.rp-frame { margin: 0; box-shadow: none; border-radius: 0; }
menu.rp-frame separator { margin: 0; }
```

La clase de esquinas rectas que había para el popover (`rp-square` en removable) ya
no hace falta: el marco de menú es recto salvo cuando se le pone `rp-round`.

### 3. Un popover que nunca se muestra, solo para leer el radio

```c
GtkWidget *pframe = g_object_new (rp_frame_get_type (),      /* nodo "popover" */
                                  "orientation", GTK_ORIENTATION_VERTICAL, NULL);
gtk_style_context_add_class (gtk_widget_get_style_context (pframe), "background");
g_object_set_data_full (G_OBJECT (win), "rp-frame-popover",
                        g_object_ref_sink (pframe), g_object_unref);
```

No tiene padre: lo mantiene vivo esa referencia. El estilo de un widget sin padre se
calcula igual, así que `gtk_style_context_get (…, "border-radius", …)` da el radio de
los popovers del tema. En removable se guardó como dato de la ventana para no tocar
`popup.h`; puede ser un campo de la estructura del popup.

### 4. Redondear solo en flotante con transparencia

```c
static void
popup_set_round (RemovablePopup *popup, gint radius)
{
    static GtkCssProvider *provider = NULL;   /* uno por proceso: mismo tema */
    GtkStyleContext *fctx = gtk_widget_get_style_context (popup->frame);

    if (radius <= 0) {
        gtk_style_context_remove_class (fctx, "rp-round");
        return;
    }
    if (!provider) {
        provider = gtk_css_provider_new ();
        gtk_style_context_add_provider_for_screen (gtk_widget_get_screen (popup->window),
            GTK_STYLE_PROVIDER (provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    }
    gchar *css = g_strdup_printf ("menu.rp-frame.rp-round { border-radius: %dpx; }", radius);
    gtk_css_provider_load_from_data (provider, css, -1, NULL);
    g_free (css);
    gtk_style_context_add_class (fctx, "rp-round");
}
```

### 5. En la función que decide el estilo (`popup_apply_style` en removable)

- Leer el radio del **popover oculto** (punto 3), no del marco visible.
- Decidir `floating` y la transparencia (`want = floating && compositor`) como antes.
- Al final: `popup_set_round (popup, want ? radius : 0);`

Se rearma en cada apertura, así un cambio de tema se ve al volver a abrir el popup.

---

## Trampas encontradas

- **La clase `background` NO va en el marco de menú.** En Greybird claro pisa el
  blanco del menú (`#fcfcfc`) con el gris de las ventanas (`#cecece`). El popover
  oculto sí la lleva: los temas escriben `popover.background`, y sin ella el radio
  podría salir distinto.
- **El radio se lee del popover**, nunca del marco de menú (casi siempre da 0, y
  Automático no flotaría nunca).
- **Esquinas redondeadas solo con transparencia**: con la ventana opaca se pintarían
  con el fondo de la ventana y se verían cuadradas (o con puntas de otro color).
- Los nombres de removable (`RemovablePopup`, `popup->frame`, `popup_apply_style`,
  clases `rp-*`) pueden ser otros en net y bluetooth: **pedir sus archivos reales
  antes de escribir el parche**, no adaptarlo a ciegas.

---

## Cómo se comprobó (repetirlo en cada plugin)

1. **Medir colores** del tema con un programa chico: un `GtkWindow` con una caja de
   nodo `popover` + clase `background`, y un `GtkMenu` con un `GtkMenuItem`; leer
   `background-color` y el color del texto, y correrlo con
   `GTK_THEME=Greybird-dark xvfb-run -a`. En Debian el tema está en el paquete
   `greybird-gtk-theme`.
2. **El código real del popup** (clases de marco, CSS, `popup_set_round`, función de
   estilo) compilado aparte con una estructura mínima, pasando por Automático,
   Flotante y Pegada con la ventana mostrada y ocultada entre medio. Comprobar en cada
   uno: nodo del marco (`menu`), color de fondo, radio, separación, transparencia, y
   que destruir la ventana no tire errores.
3. **Sin y con compositor** (`xcompmgr` dentro de `xvfb-run`). Resultado esperado:
   - Greybird-dark: fondo `#303133` en los tres estilos; flotante con compositor,
     radio 5.
   - Greybird: `#fcfcfc` en los tres.
   - Adwaita con compositor: Automático flota con radio 9; pegada recta.
4. **Captura** de la ventana flotante (`gdk_pixbuf_get_from_window`) compuesta sobre un
   fondo de color: esquinas transparentes y redondeadas, interior con el color del menú.

---

## Banco de pruebas (el usuario)

- Terminal **fish** (CachyOS): comandos sueltos compatibles con fish; lo largo, en un
  script `.sh` que se corre con `sh archivo.sh`.
- Los parches se descargan en **`~/Descargas`**; la terminal tiene que quedar en el home.
- En removable se aplican con `sh ~/Descargas/aplicar-parche.sh NN-nombre.patch`, que
  está armado para la carpeta `~/Descargas/xfce-removable-plugin` y su build con meson.
  Para net y bluetooth hay que hacer el equivalente con **su** carpeta y **su** sistema
  de compilación (net-plugin usa CMake): preguntar o pedir los archivos antes.
- Explicaciones cortas, sin jerga, comandos listos para copiar.
