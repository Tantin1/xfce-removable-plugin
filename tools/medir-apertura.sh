#!/bin/sh
# Mide cuánto tarda en abrirse el popup (parche 30).
# Uso:  sh ~/Descargas/xfce-removable-plugin/tools/medir-apertura.sh
#
# Reinicia el panel con los mensajes de medición activados, te pide que abras
# el popup unas veces, y al final deja el panel como estaba y muestra el
# resultado. El resultado queda en ~/Descargas/medicion-popup.txt

RESULTADO="$HOME/Descargas/medicion-popup.txt"
CRUDO="$(mktemp /tmp/medicion-panel.XXXXXX)"

echo "Reiniciando el panel en modo medición..."
xfce4-panel --quit >/dev/null 2>&1
sleep 2
G_MESSAGES_DEBUG=xfce-removable-plugin setsid xfce4-panel >"$CRUDO" 2>&1 </dev/null &
sleep 4

echo
echo "Listo. Ahora:"
echo "  1. Abrí el popup del plugin y cerralo. Esperá un segundo."
echo "  2. Repetilo 5 veces."
echo "  3. Volvé acá y apretá Enter."
read dummy

echo "Volviendo el panel a la normalidad..."
xfce4-panel --quit >/dev/null 2>&1
sleep 2
setsid xfce4-panel >/dev/null 2>&1 </dev/null &

# desde la primera apertura en adelante (antes están los escaneos del arranque)
grep -a "medir:" "$CRUDO" | sed 's/.*medir: //' |
    awk 'BEGIN { ok = 0 } /---- apertura ----/ { ok = 1 } ok' > "$RESULTADO"
rm -f "$CRUDO"

if [ ! -s "$RESULTADO" ]; then
    echo
    echo "No se registró ninguna apertura. ¿Se abrió el popup? ¿Está instalado el parche 30?"
    exit 1
fi

echo
cat "$RESULTADO"
echo
awk '/primer dibujo terminado/ { n++; split($0, a, "total:"); t += a[2] + 0 }
     /revisar dispositivos/    { m++; split($0, b, "ms");    s += substr(b[1], length("revisar dispositivos") + 1) + 0 }
     END {
         if (n) printf "Promedio de %d aperturas: %.1f ms hasta verse; revisar dispositivos: %.1f ms\n", n, t / n, (m ? s / m : 0)
     }' "$RESULTADO"
echo "Guardado en $RESULTADO"
