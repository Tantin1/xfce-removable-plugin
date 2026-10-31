#!/bin/sh
# Comprueba que la versión coincida en los tres lugares donde está escrita:
#   src/meson.build (project version), debian/changelog y packaging/arch/PKGBUILD.
# Con un argumento (una etiqueta, p. ej. v1.0.2) comprueba además que coincida con ella.
# Uso:  sh packaging/check-version.sh [v1.0.2]
set -eu
cd "$(dirname "$0")/.."

meson=$(sed -n "s/^[[:space:]]*version[[:space:]]*:[[:space:]]*'\([^']*\)'.*/\1/p" src/meson.build | head -n 1)
debian=$(sed -n '1s/^[^ ]* (\([^)]*\)).*/\1/p' debian/changelog)
debian=${debian%-*}                     # 1.0.2-1 → 1.0.2
arch=$(sed -n 's/^pkgver=//p' packaging/arch/PKGBUILD)

echo "meson.build:      $meson"
echo "debian/changelog: $debian"
echo "PKGBUILD:         $arch"

fail=0
[ "$meson" = "$debian" ] || { echo "ERROR: debian/changelog no coincide con meson.build"; fail=1; }
[ "$meson" = "$arch" ]   || { echo "ERROR: PKGBUILD no coincide con meson.build"; fail=1; }

if [ $# -ge 1 ] && [ -n "$1" ]; then
    tag=${1#v}
    echo "etiqueta:         $tag"
    [ "$meson" = "$tag" ] || { echo "ERROR: la etiqueta $1 no coincide con la versión $meson"; fail=1; }
fi

[ $fail -eq 0 ] && echo "OK: todas las versiones coinciden"
exit $fail
