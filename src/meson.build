project('xfce-removable-plugin', 'c',
  version : '1.0',
  default_options : ['warning_level=2']
)

# --- dependencias ---
gtk3       = dependency('gtk+-3.0',          version : '>=3.24')
xfce4panel = dependency('libxfce4panel-2.0', version : '>=4.18')
libnotify  = dependency('libnotify',         version : '>=0.7')
gudev      = dependency('gudev-1.0')
libintl    = dependency('intl', required : false)

# --- rutas ---
prefix      = get_option('prefix')
plugin_dir  = '/usr/lib/xfce4/panel/plugins'
locale_dir  = prefix / 'share' / 'locale'
gettext_pkg = 'xfce-removable-plugin'

# --- plugin ---
shared_module('xfce-removable-plugin',
  sources : ['plugin.c', 'popup.c', 'ud-dbus.c', 'ud-phones.c'],
  dependencies : [gtk3, xfce4panel, libnotify, gudev, libintl],
  c_args : [
    '-DGETTEXT_PACKAGE="' + gettext_pkg + '"',
    '-DLOCALEDIR="'       + locale_dir  + '"',
    '-DENABLE_NLS=1',
  ],
  name_prefix : 'lib',
  install      : true,
  install_dir  : plugin_dir,
)

# --- desktop entry ---
desktop_conf = configuration_data()
desktop_conf.set('PLUGIN_DIR', plugin_dir)

desktop = configure_file(
  input         : 'xfce-removable-plugin.desktop.in',
  output        : 'xfce-removable-plugin.desktop',
  configuration : desktop_conf,
)

install_data(desktop,
  install_dir : prefix / 'share' / 'xfce4' / 'panel' / 'plugins',
)

# --- traducciones ---
subdir('po')
