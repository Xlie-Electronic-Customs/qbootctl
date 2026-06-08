project('qbootctl', 'c', default_options : ['c_std=gnu11'])

cc = meson.get_compiler('c')

src = [
        'qbootctl.c',
        'bootctrl_impl.c',
        'gpt-utils.c',
        'crc32.c',
]

inc = [
        include_directories('.'),
]

executable('qbootctl', src,
        include_directories: inc,
        install: true,
        c_args: [],
)
