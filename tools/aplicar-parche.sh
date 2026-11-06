#!/bin/sh
# aplicar-parche.sh — aplica un parche del plugin, compila, instala y reinicia el panel.
#
#   sh ~/Descargas/aplicar-parche.sh 19-algo.patch
#
# Busca el parche en ~/Descargas. Tu terminal no se mueve de donde estás.
#
# Copia de referencia: el usuario usa la de ~/Descargas/aplicar-parche.sh.
# Para otro plugin, cambiar PROYECTO y los pasos de compilación (net-plugin usa CMake).

PROYECTO="$HOME/Descargas/xfce-removable-plugin"
PARCHE="$HOME/Descargas/$1"

if [ -z "$1" ]; then
    echo "Uso: sh ~/Descargas/aplicar-parche.sh NOMBRE.patch"
    echo "Parches en Descargas:"; ls "$HOME"/Descargas/*.patch 2>/dev/null
    exit 1
fi
if [ ! -f "$PARCHE" ]; then
    echo "No encuentro $PARCHE"
    echo "Parches en Descargas:"; ls "$HOME"/Descargas/*.patch 2>/dev/null
    exit 1
fi

cd "$PROYECTO" || { echo "No encuentro la carpeta $PROYECTO"; exit 1; }

# ¿ya estaba aplicado?
if patch -p1 -R --dry-run -s < "$PARCHE" >/dev/null 2>&1; then
    echo "Este parche ya estaba aplicado. No se tocó nada."
    exit 0
fi
# ¿aplica limpio? (si no, no se toca nada)
if ! patch -p1 --dry-run -s < "$PARCHE" >/dev/null 2>&1; then
    echo "ERROR: el parche no aplica. No se tocó nada. Mandale a Claude esto:"
    patch -p1 --dry-run < "$PARCHE"
    exit 1
fi

echo "== Aplicando $1"
patch -p1 < "$PARCHE" || exit 1

echo "== Compilando"
if ! meson compile -C build; then
    echo "ERROR al compilar. Mandale a Claude lo de arriba."
    exit 1
fi

echo "== Instalando (pide tu contraseña)"
sudo meson install -C build --no-rebuild || exit 1

echo "== Reiniciando el panel"
xfce4-panel -r

echo
echo "Listo: $1 aplicado e instalado."
