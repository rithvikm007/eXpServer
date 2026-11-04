#ifndef XPS_H
#define XPS_H

// Header files
#include <arpa/inet.h>
#include <assert.h>
#include <netdb.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdarg.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <time.h>
#include <dirent.h>
#include <zlib.h>

// 3rd party libraries
#include "lib/vec/vec.h" // https://github.com/rxi/vec
#include "lib/parson/parson.h" //https://github.com/kgabis/parson

// Constants
#define DEFAULT_BACKLOG 64
#define MAX_EPOLL_EVENTS 32
#define DEFAULT_BUFFER_SIZE 100000 // 100 KB
#define DEFAULT_PIPE_BUFF_THRESH 1000000 // 1 MB
#define DEFAULT_NULLS_THRESH 32

// Error constants
#define OK 0            // Success
#define E_FAIL -1       // Un-recoverable error
#define E_AGAIN -2      // Try again
#define E_NEXT -3       // Do next
#define E_NOTFOUND -4   // File not found
#define E_PERMISSION -5 // File permission denied
#define E_EOF -6        // End of file reached

// Enums
typedef enum {
  HTTP_GET,
  HTTP_HEAD,
  HTTP_POST,
  HTTP_PUT,
  HTTP_DELETE,
  HTTP_OPTIONS,
  HTTP_TRACE,
  HTTP_CONNECT,
} xps_http_method_t;

typedef enum {
  HTTP_OK = 200,
  HTTP_CREATED = 201,

  HTTP_MOVED_PERMANENTLY = 301,
  HTTP_MOVED_TEMPORARILY = 302,
  HTTP_NOT_MODIFIED = 304,
  HTTP_TEMPORARY_REDIRECT = 307,
  HTTP_PERMANENT_REDIRECT = 308,

  HTTP_BAD_REQUEST = 400,
  HTTP_UNAUTHORIZED = 401,
  HTTP_FORBIDDEN = 403,
  HTTP_NOT_FOUND = 404,
  HTTP_REQUEST_TIME_OUT = 408,
  HTTP_TOO_MANY_REQUESTS = 429,

  HTTP_INTERNAL_SERVER_ERROR = 500,
  HTTP_NOT_IMPLEMENTED = 501,
  HTTP_BAD_GATEWAY = 502,
  HTTP_SERVICE_UNAVAILABLE = 503,
  HTTP_GATEWAY_TIMEOUT = 504,
  HTTP_HTTP_VERSION_NOT_SUPPORTED = 505
} xps_http_status_code_t;

typedef enum {
  /* Request line states */
  RL_START = 0,
  RL_METHOD,
  RL_SP_AFTER_METHOD,

  RL_SCHEMA,
  RL_SCHEMA_SLASH,
  RL_SCHEMA_SLASH_SLASH,
  RL_HOST_START, // maybe Ipv4 or Ipv6
  RL_HOST,
  RL_HOST_END,
  RL_HOST_IP_LITERAL, // Ipv6; map to RL_HOST_END
  RL_PORT,
  RL_SLASH, // path
  RL_CHECK_URI,
  RL_PATH,
  RL_PATHNAME,
  RL_SP_AFTER_URI,

  RL_VERSION_START,
  RL_VERSION_H,
  RL_VERSION_HT,
  RL_VERSION_HTT,
  RL_VERSION_HTTP,
  RL_VERSION_HTTP_SLASH,
  RL_VERSION_MAJOR,
  RL_VERSION_DOT,
  RL_VERSION_MINOR,
  RL_CR,
  RL_LF,

  /* Header states */
  H_START = 0,
  H_NAME,
  H_COLON,
  H_SP_AFTER_COLON,
  H_VAL,
  H_CR,
  H_LF,
  H_LF_CR,
  H_LF_LF,
  H_LF_CR_LF,

} xps_http_parser_state_t;

// Data types
typedef unsigned char u_char;
typedef unsigned int u_int;
typedef unsigned long u_long;

// Structures
struct xps_core_s;
struct xps_loop_s;
struct xps_listener_s;
struct xps_connection_s;
struct xps_buffer_s;
struct xps_buffer_list_s;
struct xps_pipe_s;
struct xps_pipe_source_s;
struct xps_pipe_sink_s;
struct xps_file_s;
struct xps_keyval_s;
struct xps_session_s;
struct xps_http_req_s;
struct xps_http_res_s;
struct xps_config_s;
struct xps_config_server_s;
struct xps_config_listener_s;
struct xps_config_route_s;
struct xps_config_lookup_s;
struct xps_cliargs_s;
struct xps_gzip_s;

// Struct typedefs
typedef struct xps_core_s xps_core_t;
typedef struct xps_loop_s xps_loop_t;
typedef struct xps_listener_s xps_listener_t;
typedef struct xps_connection_s xps_connection_t;
typedef struct xps_buffer_s xps_buffer_t;
typedef struct xps_buffer_list_s xps_buffer_list_t;
typedef struct xps_pipe_s xps_pipe_t;
typedef struct xps_pipe_source_s xps_pipe_source_t;
typedef struct xps_pipe_sink_s xps_pipe_sink_t;
typedef struct xps_file_s xps_file_t;
typedef struct xps_keyval_s xps_keyval_t;
typedef struct xps_session_s xps_session_t;
typedef struct xps_http_req_s xps_http_req_t;
typedef struct xps_http_res_s xps_http_res_t;
typedef struct xps_config_s xps_config_t;
typedef struct xps_config_server_s xps_config_server_t;
typedef struct xps_config_listener_s xps_config_listener_t;
typedef struct xps_config_route_s xps_config_route_t;
typedef struct xps_config_lookup_s xps_config_lookup_t;
typedef struct xps_cliargs_s xps_cliargs_t;
typedef struct xps_gzip_s xps_gzip_t;

// Function typedefs
typedef void (*xps_handler_t)(void *ptr);

// xps headers
#include "core/xps_core.h"
#include "core/xps_loop.h"
#include "core/xps_pipe.h"
#include "disk/xps_gzip.h"
#include "core/xps_session.h"
#include "disk/xps_file.h"
#include "disk/xps_mime.h"
#include "http/xps_http.h"
#include "http/xps_http_req.h"
#include "http/xps_http_res.h"
#include "network/xps_connection.h"
#include "network/xps_listener.h"
#include "network/xps_upstream.h"
#include "utils/xps_logger.h"
#include "utils/xps_utils.h"
#include "utils/xps_buffer.h"
#include "utils/xps_cliargs.h"
#include "config/xps_config.h"
#include "disk/xps_directory.h"

#endif