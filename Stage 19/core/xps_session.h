#ifndef XPS_SESSION_H
#define XPS_SESSION_H

#include "../xps.h"

struct xps_session_s {
  xps_core_t *core;
  xps_connection_t *client;
  xps_connection_t *upstream;
  bool upstream_connected;
  bool upstream_error_res_set;
  u_long upstream_write_bytes;
  xps_file_t *file;
  xps_gzip_t *gzip;
  xps_pipe_source_t *client_source;
  xps_pipe_sink_t *client_sink;
  xps_pipe_source_t *upstream_source;
  xps_pipe_sink_t *upstream_sink;
  xps_pipe_sink_t *file_sink;
  xps_buffer_t *to_client_buff;
  xps_buffer_t *from_client_buff;
  xps_http_req_t *http_req;
  xps_config_lookup_t *lookup;
};

xps_session_t *xps_session_create(xps_core_t *core, xps_connection_t *client);
void xps_session_destroy(xps_session_t *session);
void client_source_handler(void *ptr);
void client_source_close_handler(void *ptr);
void client_sink_handler(void *ptr);
void client_sink_close_handler(void *ptr);
void upstream_source_handler(void *ptr);  
void upstream_source_close_handler(void *ptr);
void upstream_sink_handler(void *ptr);
void upstream_sink_close_handler(void *ptr);
void file_sink_handler(void *ptr);
void file_sink_close_handler(void *ptr);
void session_check_destroy(xps_session_t *session);
void xps_session_destroy(xps_session_t *session);
void set_to_client_buff(xps_session_t *session, xps_buffer_t *buff);
void set_from_client_buff(xps_session_t *session, xps_buffer_t *buff);
void upstream_error_res(xps_session_t *session);

#endif