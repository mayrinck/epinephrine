gnome = import('gnome')

resources = gnome.compile_resources('epinephrine-resources',
  'resources.gresource.xml',
  source_dir: '../data',
  c_name: 'epinephrine',
)

sources = files(
  'main.c',
  'application.c',
  'window.c',
  'command-builder.c',
  'epi-privilege.c',
  'epi-i18n.c',
  'usb-ids.c',
  'pages/page-dashboard.c',
  'pages/page-games.c',
  'pages/page-hardware.c',
  'pages/page-cpu.c',
  'pages/page-keyboard.c',
  'pages/page-mouse.c',
  'pages/page-gamescope.c',
  'pages/page-gamemode.c',
  'pages/page-utilities.c',
  'pages/page-mangohud.c',
  'pages/page-settings.c',
)

executable('epinephrine',
  sources, resources,
  dependencies: [gtk4_dep, libadwaita_dep, m_dep, fontconfig_dep],
  install: true,
)

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
