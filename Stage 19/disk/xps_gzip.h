
#ifndef XPS_GZIP_H

#define XPS_GZIP_H

#include "../xps.h"

struct xps_gzip_s {

    z_stream stream;
    xps_pipe_source_t *source;
    xps_pipe_sink_t *sink;
    xps_buffer_t *transfer_buff;
    size_t total_in_len;
    size_t total_out_len;

};

xps_gzip_t *xps_gzip_create(int level);

void xps_gzip_destroy(xps_gzip_t *gzip);

#endif