gcc -g -o xps \
    main.c \
    config/xps_config.c \
    disk/xps_file.c \
    disk/xps_mime.c \
    disk/xps_directory.c \
    disk/xps_gzip.c \
    core/xps_core.c \
    core/xps_loop.c \
    core/xps_pipe.c \
    core/xps_session.c \
    lib/vec/vec.c \
    lib/parson/parson.c \
    http/xps_http_req.c \
    http/xps_http_res.c \
    http/xps_http.c \
    network/xps_connection.c \
    network/xps_listener.c \
    network/xps_upstream.c \
    utils/xps_cliargs.c \
    utils/xps_logger.c \
    utils/xps_utils.c \
    utils/xps_buffer.c \
    -lz