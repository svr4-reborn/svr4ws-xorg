project('svr4ws-xorg', 'c')

xorg_server = dependency('xorg-server')

shared_module('svr4ws_drv',
    'src/svr4ws_driver.c',
    dependencies : xorg_server,
    name_prefix : '',
    install : true,
    install_dir : join_paths(get_option('libdir'), 'xorg/modules/drivers'))

shared_module('svr4xqueue_drv',
    'src/svr4xqueue.c',
    dependencies : xorg_server,
    name_prefix : '',
    install : true,
    install_dir : join_paths(get_option('libdir'), 'xorg/modules/input'))

install_data('conf/40-svr4xqueue.conf', install_dir : '/etc/X11/xorg.conf.d')
install_data('conf/10-svr4ws.conf', install_dir : '/etc/X11/xorg.conf.d')
