project('bspwwm', 'c', version: '0.1.0',
  default_options: ['c_std=c11', 'warning_level=3'])

cc = meson.get_compiler('c')
wlroots = dependency('wlroots-0.20', version: ['>=0.20.0', '<0.21.0'])
wayland = dependency('wayland-server')
xkb = dependency('xkbcommon')
math = cc.find_library('m')
add_project_arguments('-D_POSIX_C_SOURCE=200809L', '-DWLR_USE_UNSTABLE', language: 'c')
inc = include_directories('wayland')
bsp = static_library('bsp', 'wayland/bsp.c', include_directories: inc)
compositor = executable('bspwwm', ['wayland/server.c', 'wayland/ipc.c'],
  include_directories: inc, link_with: bsp,
  dependencies: [wlroots, wayland, xkb, math], install: true)
client = executable('bspc', 'wayland/client.c', install: true)
test('bsp', executable('bsp-test', 'tests/bsp_test.c',
  include_directories: inc, link_with: bsp, dependencies: math))
test('platform', executable('platform-test', ['tests/platform_test.c', 'wayland/ipc.c'],
  include_directories: inc, link_with: bsp, dependencies: [wlroots, wayland, xkb, math]))
install_data('contrib/freedesktop/bspwwm.desktop',
  install_dir: get_option('datadir') / 'wayland-sessions')
install_data('examples/bspwwmrc', install_dir: get_option('datadir') / 'doc/bspwwm/examples')
install_data('README.md', 'doc/WAYLAND.md', 'LICENSE', 'LICENSE.wlroots',
  install_dir: get_option('datadir') / 'doc/bspwwm')

protocols = dependency('wayland-protocols')
scanner = find_program('wayland-scanner')
xdg_xml = protocols.get_variable(pkgconfig: 'pkgdatadir') / 'stable/xdg-shell/xdg-shell.xml'
xdg_header = custom_target('xdg-shell-client-header', input: xdg_xml,
  output: 'xdg-shell-client-protocol.h', command: [scanner, 'client-header', '@INPUT@', '@OUTPUT@'])
xdg_code = custom_target('xdg-shell-client-code', input: xdg_xml,
  output: 'xdg-shell-protocol.c', command: [scanner, 'private-code', '@INPUT@', '@OUTPUT@'])
test_client = executable('wayland-test-client', ['tests/wayland_client.c', xdg_header, xdg_code],
  dependencies: dependency('wayland-client'))
python = find_program('python3')
test('headless', python, args: [files('tests/headless.py'), compositor, client, test_client], timeout: 90)
