#!/bin/sh
# Arma el .deb para la distro donde se ejecuta (Debian 12, Debian 13…).
# Uso, desde cualquier carpeta:   sh packaging/build-deb.sh
# El .deb queda en la carpeta que contiene al proyecto, con la distro en el nombre:
#   xfce-removable-plugin_1.0.0-1_debian12_amd64.deb
set -eu

cd "$(dirname "$0")/.."
pkg=xfce-removable-plugin

if ! dpkg-checkbuilddeps 2>/dev/null; then
    echo "Faltan dependencias de compilación. Instalalas con:"
    echo "  sudo apt install build-essential debhelper meson pkgconf gettext \\"
    echo "       libgtk-3-dev libxfce4panel-2.0-dev libnotify-dev libgudev-1.0-dev"
    exit 1
fi

chmod +x debian/rules
dpkg-buildpackage -us -uc -b

. /etc/os-release
ver=$(dpkg-parsechangelog -S Version)
arch=$(dpkg --print-architecture)
out="../${pkg}_${ver}_${ID}${VERSION_ID:-}_${arch}.deb"
mv "../${pkg}_${ver}_${arch}.deb" "$out"

# limpiar lo que deja dpkg-buildpackage (dentro del proyecto y al lado)
debian/rules clean >/dev/null
rm -f "../${pkg}_${ver}_${arch}.buildinfo" "../${pkg}_${ver}_${arch}.changes" \
      "../${pkg}-dbgsym_${ver}_${arch}.deb" "../${pkg}-dbgsym_${ver}_${arch}.ddeb"

echo
echo "Listo: $(cd .. && pwd)/$(basename "$out")"
echo "Instalar con:  sudo apt install $(cd .. && pwd)/$(basename "$out")"
