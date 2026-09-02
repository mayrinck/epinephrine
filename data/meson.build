desktop_file = i18n.merge_file(
  input: 'io.github.mayrinck.epinephrine.desktop.in',
  output: 'io.github.mayrinck.epinephrine.desktop',
  type: 'desktop',
  po_dir: '../po',
  install: true,
  install_dir: join_paths(get_option('datadir'), 'applications'),
)

appstream_file = i18n.merge_file(
  input: 'io.github.mayrinck.epinephrine.metainfo.xml.in',
  output: 'io.github.mayrinck.epinephrine.metainfo.xml',
  type: 'xml',
  po_dir: '../po',
  install: true,
  install_dir: join_paths(get_option('datadir'), 'metainfo'),
)

install_data('io.github.mayrinck.epinephrine.svg',
  install_dir: join_paths(get_option('datadir'), 'icons', 'hicolor', 'scalable', 'apps'),
)

install_data('../database/GPU_INDEX.md', '../database/JOY_INDEX.md',
  '../database/RAM_INDEX.md',
  install_dir: join_paths(get_option('datadir'), 'epinephrine', 'database'),
)
