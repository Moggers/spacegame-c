project('opendom', 'c', default_options: 'c_std=c99')

cc = meson.get_compiler('c')
libm = cc.find_library('m', required : false)

vulkan = dependency('vulkan')
glfw = dependency('glfw3')
assimp = dependency('assimp')

executable('main', 'src/main.c', dependencies: [vulkan, glfw, libm, assimp])
