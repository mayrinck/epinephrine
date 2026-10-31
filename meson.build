project('epinephrine', 'c',
  version: '0.1.0',
  license: 'MIT',
  meson_version: '>= 1.0.0',
  default_options: [
    'c_std=c17',
    'warning_level=2',
  ],
)

gtk4_dep = dependency('gtk4', version: '>= 4.12')
libadwaita_dep = dependency('libadwaita-1', version: '>= 1.5')
gdk_pixbuf_dep = dependency('gdk-pixbuf-2.0', version: '>= 2.42')
fontconfig_dep = dependency('fontconfig')
m_dep = meson.get_compiler('c').find_library('m', required: true)

i18n = import('i18n')

add_project_arguments(
  '-DGETTEXT_PACKAGE="@0@"'.format(meson.project_name()),
  '-DLOCALEDIR="@0@"'.format(join_paths(get_option('prefix'), get_option('localedir'))),
  '-DLIBEXECDIR="@0@"'.format(join_paths(get_option('prefix'), get_option('libexecdir'))),
  '-DDATADIR="@0@"'.format(join_paths(get_option('prefix'), get_option('datadir'))),
  language: 'c',
)

subdir('data')
subdir('src')
subdir('po')

python3 = find_program('python3', required: false)
if python3.found()
  test('admin-policy', python3, args: [files('tests/test_admin_helper.py')])
  test('game-metadata', python3, args: [files('tests/test_game_metadata.py')])
endif
