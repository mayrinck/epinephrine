gnome = import('gnome')

resources = gnome.compile_resources('epinephrine-resources',
  'resources.gresource.xml',
  source_dir: '../data',
  c_name: 'epinephrine',
)

sources = files(
  'application.c',
  'window.c',
  'command-builder.c',
  'hydra-ldb.c',
  'epi-privilege.c',
  'epi-i18n.c',
  'epi-ratbagd.c',
  'usb-ids.c',
  'pages/page-dashboard.c',
  'pages/page-games.c',
  'pages/game-details.c',
  'pages/game-artwork.c',
  'pages/game-icons.c',
  'pages/game-store.c',
  'pages/game-importers.c',
  'pages/page-hardware.c',
  'pages/hw-gpu.c',
  'pages/hw-monitor.c',
  'pages/hw-gpu-detail.c',
  'pages/hw-controller.c',
  'pages/hw-detail.c',
  'pages/hw-devices.c',
  'pages/hw-device-list.c',
  'pages/page-cpu.c',
  'pages/page-keyboard.c',
  'pages/page-mouse.c',
  'pages/page-gamescope.c',
  'pages/page-gamemode.c',
  'pages/page-utilities.c',
  'pages/page-mangohud.c',
  'pages/page-settings.c',
)

core_deps = [gtk4_dep, libadwaita_dep, gdk_pixbuf_dep, m_dep, fontconfig_dep]
core = static_library('epinephrine-core', sources, resources, dependencies: core_deps)
executable('epinephrine', 'main.c', link_whole: core, dependencies: core_deps, install: true)
ui_test = executable('test-library-ui', '../tests/test-library-ui.c',
  include_directories: include_directories('.'), link_whole: core, dependencies: core_deps)
test('library-ui', ui_test)

executable('epi-hidraw',
  'epi-hidraw.c',
  install: true,
)

install_data('../lang/pt_BR.md',
  install_dir: join_paths(get_option('datadir'), 'epinephrine', 'lang'),
)
install_data('../lang/en_US.md',
  install_dir: join_paths(get_option('datadir'), 'epinephrine', 'lang'),
)
install_data('../lang/es_ES.md',
  install_dir: join_paths(get_option('datadir'), 'epinephrine', 'lang'),
)

install_data('../scripts/hydra_games.py',
  install_dir: join_paths(get_option('datadir'), 'epinephrine', 'scripts'),
)

icon_test = executable('test-game-icons', '../tests/test-game-icons.c',
  'pages/game-icons.c', include_directories: include_directories('.'),
  dependencies: [gtk4_dep, libadwaita_dep, gdk_pixbuf_dep])
test('game-icons', icon_test)

executable('epi-admin', 'epi-admin.c', resources, dependencies: [gtk4_dep], install: true, install_dir: join_paths(get_option('libexecdir'), 'epinephrine'))
