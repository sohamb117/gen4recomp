# Native glTF → NSBMD encoder. One static mesh, optional later TEX0.
# yyjson is already a wrap (dataproc); native:true matches that library.

nitromdl_cflags = [
    '-std=gnu17',
    '-O2',
    '-Wall',
    '-Wextra',
    '-Wno-sign-conversion',
]

cc = meson.get_compiler('c', native: true)

nitromdl_exe = executable(
    'nitromdl',
    sources: files('nitromdl.c'),
    c_args: nitromdl_cflags,
    dependencies: [
        dependency('yyjson'),
        cc.find_library('m', required: false),
    ],
    native: true,
    install: false,
)
