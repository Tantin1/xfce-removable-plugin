project('xfce-removable-plugin', 'c',
  version         : '1.0.0',
  meson_version   : '>= 0.60',
  default_options : ['warning_level=2'],
)

# --- dependencias (mínimo: Debian 12 / Xfce 4.18) ---
gtk3       = dependency('gtk+-3.0',          version : '>=3.24')
xfce4panel = dependency('libxfce4panel-2.0', version : '>=4.18')
libnotify  = dependency('libnotify',         version : '>=0.7')
gudev      = dependency('gudev-1.0')
libintl    = dependency('intl', required : false)

# --- rutas ---
prefix      = get_option('prefix')
locale_dir  = prefix / get_option('localedir')
gettext_pkg = meson.project_name()

# El .so va donde lo busca el panel instalado: <libdir de libxfce4panel>/xfce4/panel/plugins
#   Arch / CachyOS        → /usr/lib/xfce4/panel/plugins
#   Debian 12 / 13 (amd64) → /usr/lib/x86_64-linux-gnu/xfce4/panel/plugins
plugin_dir = get_option('plugin-dir')
if plugin_dir == ''
  plugin_dir = xfce4panel.get_variable(pkgconfig : 'libdir') / 'xfce4' / 'panel' / 'plugins'
endif
message('El plugin se instalará en: ' + plugin_dir)

# --- plugin ---
shared_module('xfce-removable-plugin',
  sources : ['plugin.c', 'popup.c', 'ud-dbus.c', 'ud-phones.c'],
  dependencies : [gtk3, xfce4panel, libnotify, gudev, libintl],
  c_args : [
    '-DGETTEXT_PACKAGE="' + gettext_pkg + '"',
    '-DLOCALEDIR="'       + locale_dir  + '"',
    '-DPACKAGE_VERSION="' + meson.project_version() + '"',
    '-DENABLE_NLS=1',
  ],
  name_prefix : 'lib',
  install      : true,
  install_dir  : plugin_dir,
)

# --- desktop entry (nombre y descripción traducidos desde po/) ---
i18n = import('i18n')
i18n.merge_file(
  input       : 'xfce-removable-plugin.desktop.in',
  output      : 'xfce-removable-plugin.desktop',
  type        : 'desktop',
  po_dir      : 'po',
  install     : true,
  install_dir : get_option('datadir') / 'xfce4' / 'panel' / 'plugins',
)

# --- traducciones ---
subdir('po')
