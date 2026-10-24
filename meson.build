project('xshell', 'cpp',
  version : '0.1.0',
  default_options : ['warning_level=3', 'cpp_std=c++23']
)

inc_dir = include_directories(
  'include',
  '../../sdk/xkit/include',
  '../../include',
  '../../sdk/xrender/include',
  '../xserver/include'
)

# For prototyping, we compile the necessary framework files directly.
# In production, xkit would be a shared library (.so)
src_files = [
  'src/main.cc',
  'src/shell_panels.cc',
  '../../sdk/xkit/src/widget.cc',
  '../../sdk/xkit/src/audio_player.cc',
  '../../sdk/xrender/src/display_list.cc',
  '../../sdk/xrender/src/entity.cc',
  '../xserver/src/ipc_socket.cc'
]

# xrender sources (compiled directly for prototyping) pull in FreeType/HarfBuzz/
# Fontconfig via font_manager.h, so xshell needs those include+link flags too.
xshell_deps = [
  dependency('freetype2'),
  dependency('harfbuzz'),
  dependency('fontconfig'),
  dependency('libpng'),
  dependency('vulkan'),
]

executable('xshell',
  src_files,
  include_directories: inc_dir,
  dependencies: xshell_deps,
  install : true
)
