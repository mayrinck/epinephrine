project('epinephrine', 'c',
  version: '0.1.0',
  license: 'MIT',
  meson_version: '>= 1.0.0',
  default_options: [
    'c_std=c17',
    'warning_level=2',
  ],
)

gtk4_dep = dependency('gtk4', version: '>= 4.10')
libadwaita_dep = dependency('libadwaita-1', version: '>= 1.4')
fontconfig_dep = dependency('fontconfig')
m_dep = meson.get_compiler('c').find_library('m', required: true)

i18n = import('i18n')

add_project_arguments(
  '-DGETTEXT_PACKAGE="@0@"'.format(meson.project_name()),
  '-DLOCALEDIR="@0@"'.format(join_paths(get_option('prefix'), get_option('localedir'))),
  '-DDATADIR="@0@"'.format(join_paths(get_option('prefix'), get_option('datadir'))),
  language: 'c',
)

subdir('data')
subdir('src')
subdir('po')
