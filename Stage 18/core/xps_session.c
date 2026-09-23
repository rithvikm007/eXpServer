#include "../xps.h"

void session_process_request(xps_session_t *session);

xps_session_t * xps_session_create(xps_core_t * core, xps_connection_t * client) {
    /* validate parameters */
    assert(core != NULL);
    assert(client != NULL);

    // Alloc memory for session instance
    xps_session_t * session = (xps_session_t * ) malloc(sizeof(xps_session_t));
    if (session == NULL) {
        logger(LOG_ERROR, "xps_session_create()", "malloc() failed for 'session'");
        return NULL;
    }

    session -> client_source = xps_pipe_source_create(session, client_source_handler, client_source_close_handler);
    session -> client_sink = xps_pipe_sink_create(session, client_sink_handler, client_sink_close_handler);
    session -> upstream_source = xps_pipe_source_create(session, upstream_source_handler, upstream_source_close_handler);
    session -> upstream_sink = xps_pipe_sink_create(session, upstream_sink_handler, upstream_sink_close_handler);
    session -> file_sink = xps_pipe_sink_create(session, file_sink_handler, file_sink_close_handler);

    if (!(session -> client_source && session -> client_sink && session -> upstream_source && session -> upstream_sink && session -> file_sink)) {
        logger(LOG_ERROR, "xps_session_create()", "failed to create some sources/sinks");

        if (session -> client_source) {
            xps_pipe_source_destroy(session -> client_source);
        }
        if (session -> client_sink) {
            xps_pipe_sink_destroy(session -> client_sink);
        }
        if (session -> upstream_source) {
            xps_pipe_source_destroy(session -> upstream_source);
        }
        if (session -> upstream_sink) {
            xps_pipe_sink_destroy(session -> upstream_sink);
        }
        if (session -> file_sink) {
            xps_pipe_sink_destroy(session -> file_sink);
        }

        free(session);
        return NULL;
    }

    // Init values
    session -> core = core;
    session -> client = client;
    session -> upstream = NULL;
    session -> upstream_connected = false;
    session -> upstream_error_res_set = false;
    session -> upstream_write_bytes = 0;
    session -> file = NULL;
    session -> to_client_buff = NULL;
    session -> from_client_buff = NULL;
    session -> client_sink -> ready = true;
    session -> upstream_sink -> ready = true;
    session -> file_sink -> ready = true;
    session -> http_req = NULL;

    /*NOTE: We will be adding list of sessions as vec_void_t sessions to xps_core_s in xps_core module below*/
    // Add current session to core->sessions
    vec_push( & (core -> sessions), session);

    // Attach client
    if (xps_pipe_create(core, DEFAULT_PIPE_BUFF_THRESH, client -> source, session -> client_sink) == NULL || xps_pipe_create(core, DEFAULT_PIPE_BUFF_THRESH, session -> client_source, client -> sink) == NULL) {
        logger(LOG_ERROR, "xps_session_create()", "failed to create client pipes");

        if (session -> client_source) {
            xps_pipe_source_destroy(session -> client_source);
        }
        if (session -> client_sink) {
            xps_pipe_sink_destroy(session -> client_sink);
        }
        if (session -> upstream_source) {
            xps_pipe_source_destroy(session -> upstream_source);
        }
        if (session -> upstream_sink) {
            xps_pipe_sink_destroy(session -> upstream_sink);
        }
        if (session -> file_sink) {
            xps_pipe_sink_destroy(session -> file_sink);
        }

        free(session);
        return NULL;
    }

    logger(LOG_DEBUG, "xps_session_create()", "created session");

    return session;
}

void client_source_handler(void *ptr) {
    assert(ptr != NULL);

    xps_pipe_source_t *source = ptr;
    xps_session_t *session = source->ptr;

    logger(LOG_DEBUG, "client_source_handler()", "before return: ready=%d active=%d", source->ready, source->active);

    logger(LOG_DEBUG, "client_source_handler()", "session=%p to_client_buff=%p", (void *)session, (void *)session->to_client_buff);

    if (session->to_client_buff == NULL) {
        logger(LOG_ERROR, "client_source_handler()", "previous data not yet sent to client");
        return;
    }

    if (xps_pipe_source_write(source, session->to_client_buff) != OK) {
        logger(LOG_ERROR, "client_source_handler()", "xps_pipe_source_write() failed");
        return;
    }

    logger(LOG_DEBUG, "client_source_handler()", "sent %zu bytes to client", session->to_client_buff->len);

    xps_buffer_destroy(session->to_client_buff);

    set_to_client_buff(session, NULL);
    session_check_destroy(session);
}

void client_source_close_handler(void * ptr) {
    assert(ptr != NULL);

    xps_pipe_source_t * source = (xps_pipe_source_t * ) ptr;
    xps_session_t * session = (xps_session_t * ) source -> ptr;

    session_check_destroy(session);
}

void client_sink_handler(void * ptr) {
    assert(ptr != NULL);

    xps_pipe_sink_t * sink = (xps_pipe_sink_t * ) ptr;
    xps_session_t * session = (xps_session_t * ) sink -> ptr;

    size_t in_pipe = sink -> pipe -> buff_list -> len;
    if (in_pipe == 0) {
        logger(LOG_DEBUG, "client_sink_handler()", "no data to be read from pipe");
        return;
    }
    xps_buffer_t * buff = xps_pipe_sink_read(sink, in_pipe);
    if (buff == NULL) {
        logger(LOG_ERROR, "client_sink_handler()", "xps_pipe_sink_read() failed");
        return;
    }

    if (session->http_req == NULL) { //http requset is not recieved till now//
        int error;
        // create http_req for the buff read from pipe and destroy the buff
        xps_http_req_t *http_req = xps_http_req_create(session->core, buff, &error);
        if (error != OK) {
            // process the session and return
            error = HTTP_BAD_REQUEST;
            logger(LOG_ERROR, "client_sink_handler()", "xps_http_req_create() failed with error code %d", error);
            session_process_request(session);
            return;
        }
        // handle E_AGAIN
        if(error == E_AGAIN) {
            logger(LOG_DEBUG, "client_sink_handler()", "xps_http_req_create() returned E_AGAIN, waiting for more data");
            return;
        }
        session->http_req = http_req;
        logger(LOG_DEBUG, "client_sink_handler()", "http_req created successfully");
        // serialize http_req into buffer http_req_buff
        xps_buffer_t *http_req_buff = xps_http_req_serialize(http_req);
        // set http_req_buff to from_client_buff and clear the pipe
        set_from_client_buff(session, http_req_buff);
        size_t to_clear = sink->pipe->buff_list->len;
        xps_pipe_sink_clear(sink, to_clear);
        // process the session
        session_process_request(session);
    } else {
        set_from_client_buff(session, buff);
        xps_pipe_sink_clear(sink, buff->len);
    }
}

void client_sink_close_handler(void * ptr) {

   
    assert(ptr != NULL);
    xps_pipe_sink_t * sink = (xps_pipe_sink_t * ) ptr;
    xps_session_t * session = (xps_session_t * ) sink -> ptr;
    assert(session != NULL);
    session_check_destroy(session);

}

void upstream_source_handler(void * ptr) {
    /*assert*/
    assert(ptr != NULL);

    /*set ptr as source*/
    xps_pipe_source_t * source = (xps_pipe_source_t * ) ptr;

    /*set session as source->ptr*/
    xps_session_t * session = (xps_session_t * ) source -> ptr;

    if (session -> from_client_buff == NULL) {
        logger(LOG_ERROR, "upstream_source_handler()", "no data to be written to upstream");
		source->ready = false;
        return;
    }

    if (xps_pipe_source_write(source, session -> from_client_buff) != OK) {
        logger(LOG_ERROR, "upstream_source_handler()", "xps_pipe_source_write() failed");
        return;
    }

    // Checking if upstream is connected
    if (session -> upstream_connected == false) {
        session -> upstream_write_bytes += session -> from_client_buff -> len;
        if (session -> upstream_write_bytes > session -> upstream_source -> pipe -> buff_list -> len) {
            session -> upstream_connected = true;
        }
    }

    xps_buffer_destroy(session -> from_client_buff);
	source->ready = false;

    set_from_client_buff(session, NULL);
    session_check_destroy(session);
}

void upstream_source_close_handler(void * ptr) {
   
    assert(ptr != NULL);
    xps_pipe_source_t * source = (xps_pipe_source_t * ) ptr;
    xps_session_t * session = (xps_session_t * ) source -> ptr;
    assert(session != NULL);

    if ((session -> upstream_connected == false) && (session -> upstream_error_res_set == false)) {
        upstream_error_res(session);
    }

   
    session_check_destroy(session);
}

void upstream_sink_handler(void * ptr) {
   
    assert(ptr != NULL);
    xps_pipe_sink_t * sink = (xps_pipe_sink_t * ) ptr;
    xps_session_t * session = (xps_session_t * ) sink -> ptr;
    assert(session != NULL);

    session -> upstream_connected = true;

    size_t in_pipe = sink -> pipe -> buff_list -> len;
    if (in_pipe == 0) {
        logger(LOG_DEBUG, "upstream_sink_handler()", "no data to be read from pipe");
        return;
    }
    xps_buffer_t * buff = xps_pipe_sink_read(sink, in_pipe);
    if (buff == NULL) {
        logger(LOG_ERROR, "upstream_sink_handler()", "xps_pipe_sink_read() failed");
        return;
    }

    set_to_client_buff(session, buff);
    xps_pipe_sink_clear(sink, in_pipe);
}

void upstream_sink_close_handler(void * ptr) {
   
    assert(ptr != NULL);
    xps_pipe_sink_t * sink = (xps_pipe_sink_t * ) ptr;
    xps_session_t * session = (xps_session_t * ) sink -> ptr;
    assert(session != NULL);

    if ((session -> upstream_connected == false) && (session -> upstream_error_res_set == false)) {
        upstream_error_res(session);
    }

   
    session_check_destroy(session);
}

void upstream_error_res(xps_session_t * session) {
    assert(session != NULL);

    session -> upstream_error_res_set = true;
}

void file_sink_handler(void * ptr) {
   
    assert(ptr != NULL);
    xps_pipe_sink_t * sink = (xps_pipe_sink_t * ) ptr;
    xps_session_t * session = (xps_session_t * ) sink -> ptr;
    assert(session != NULL);

    size_t in_pipe = sink -> pipe -> buff_list -> len;
    if (in_pipe == 0) {
        logger(LOG_DEBUG, "file_sink_handler()", "no data to be read from pipe");
        return;
    }
    xps_buffer_t * buff = xps_pipe_sink_read(sink, in_pipe);
    if (buff == NULL) {
        logger(LOG_ERROR, "file_sink_handler()", "xps_pipe_sink_read() failed");
        return;
    }
    if (buff == NULL) {
        logger(LOG_ERROR, "file_sink_handler()", "xps_pipe_sink_read() failed");
        return;
    }

    set_to_client_buff(session, buff);
    xps_pipe_sink_clear(sink, in_pipe);
}

void file_sink_close_handler(void * ptr) {

   
    assert(ptr != NULL);
    xps_pipe_sink_t * sink = (xps_pipe_sink_t * ) ptr;
    xps_session_t * session = (xps_session_t * ) sink -> ptr;
    assert(session != NULL);

    session_check_destroy(session);

}

void set_to_client_buff(xps_session_t * session, xps_buffer_t * buff) {
    /* validate parameters */
    assert(session != NULL);

    session -> to_client_buff = buff;

    if (buff == NULL) {
        session -> client_source -> ready = false;
        session -> upstream_sink -> ready = true;
        session -> file_sink -> ready = true;
    } else {
        session -> client_source -> ready = true;
        session -> upstream_sink -> ready = false;
        session -> file_sink -> ready = false;
    }
}

void set_from_client_buff(xps_session_t * session, xps_buffer_t * buff) {
    /* validate parameters */
    assert(session != NULL);

    session -> from_client_buff = buff;

    if (buff == NULL) {
        session -> client_sink -> ready = true;
        session -> upstream_source -> ready = false;
    } else {
        session -> client_sink -> ready = false;
        session -> upstream_source -> ready = true;
    }
}

void session_check_destroy(xps_session_t * session) {
    /* validate parameters */
    assert(session != NULL);

    bool c2u_flow = session -> upstream_source -> active && (session -> client_sink -> active || session -> from_client_buff);

    bool u2c_flow = session -> client_source -> active && (session -> upstream_sink -> active || session -> to_client_buff);

    bool f2c_flow = session -> client_source -> active && (session -> file_sink -> active || session -> to_client_buff);

    bool flowing = c2u_flow || u2c_flow || f2c_flow;

    if (!flowing) {
        xps_session_destroy(session);
    }
}

void xps_session_destroy(xps_session_t * session) {
    /* validate parameters */
    assert(session != NULL);

    /* destroy client_source, client_sink, upstream_source, upstream_sink and file_sink attached to session */
    if (session -> client_source) {
        xps_pipe_source_destroy(session -> client_source);
    }
    if (session -> client_sink) {
        xps_pipe_sink_destroy(session -> client_sink);
    }
    if (session -> upstream_source) {
        xps_pipe_source_destroy(session -> upstream_source);
    }
    if (session -> upstream_sink) {
        xps_pipe_sink_destroy(session -> upstream_sink);
    }
    if (session -> file_sink) {
        xps_pipe_sink_destroy(session -> file_sink);
    }

    if (session -> to_client_buff != NULL) {
        xps_buffer_destroy(session -> to_client_buff);
    }
    if (session -> from_client_buff != NULL) {
        xps_buffer_destroy(session -> from_client_buff);
    }

    // Set NULL in core's list of sessions
    for (int i = 0; i < session -> core -> sessions.length; i++) {
        if (session -> core -> sessions.data[i] == session) {
            session -> core -> sessions.data[i] = NULL;
            session -> core -> n_null_sessions++;
            break;
        }
    }

    if (session->http_req != NULL) {
        xps_http_req_destroy(session->core, session->http_req);
    }
   
    free(session);

    logger(LOG_DEBUG, "xps_session_destroy()", "destroyed session");
}

void session_process_request(xps_session_t *session) {
    assert(session != NULL);

    xps_http_res_t *http_res = NULL;

    // BAD REQUEST
    if (session->http_req == NULL || session->http_req->path == NULL) {
        http_res = xps_http_res_create(session->core, HTTP_BAD_REQUEST);
        if (http_res == NULL) {
            logger(LOG_ERROR, "session_process_request()", "xps_http_res_create() failed");
            return;
        }
        xps_buffer_t *buff = xps_http_res_serialize(http_res);
        if (buff == NULL) {
            logger(LOG_ERROR, "session_process_request()", "xps_http_res_serialize() failed");
            xps_http_res_destroy(http_res);
            return;
        }
        set_to_client_buff(session, buff);
        xps_http_res_destroy(http_res);
        return;
    }

    // Config lookup for the requested path
    int error;
    xps_config_lookup_t *lookup = xps_config_lookup(session->core->config, session->http_req, session->client, &error);

    if(lookup == NULL) {
        if(error == E_NOTFOUND) {
            http_res = xps_http_res_create(session->core, HTTP_NOT_FOUND);
        } else if(error == E_PERMISSION) {
            http_res = xps_http_res_create(session->core, HTTP_FORBIDDEN);
        } else {
            http_res = xps_http_res_create(session->core, HTTP_INTERNAL_SERVER_ERROR);
        }
        xps_buffer_t *buff = xps_http_res_serialize(http_res);
        set_to_client_buff(session, buff);
        xps_http_res_destroy(http_res);
        return;
    }

    // IP whitelist / blacklist check
    const char *client_ip = session->client->remote_ip;

    // Whitelist: if present, client IP must be in the whitelist
    if (lookup->ip_whitelist.length > 0) {
        bool allowed = false;

        for (size_t i = 0; i < lookup->ip_whitelist.length; i++) {
            const char *allowed_ip = lookup->ip_whitelist.data[i];

            if (strcmp(client_ip, allowed_ip) == 0) {
                allowed = true;
                break;
            }
        }

        if (!allowed) {
            http_res = xps_http_res_create(session->core, HTTP_FORBIDDEN);
            if (http_res == NULL) {
                logger(LOG_ERROR, "session_process_request()", "xps_http_res_create() failed");
                return;
            }

            xps_buffer_t *buff = xps_http_res_serialize(http_res);
            if (buff == NULL) {
                logger(LOG_ERROR, "session_process_request()", "xps_http_res_serialize() failed");
                xps_http_res_destroy(http_res);
                return;
            }

            set_to_client_buff(session, buff);
            xps_http_res_destroy(http_res);
            return;
        }
    }

    // Blacklist: if present, client IP must NOT be in the blacklist
    if (lookup->ip_blacklist.length > 0) {
        for (size_t i = 0; i < lookup->ip_blacklist.length; i++) {
            const char *blocked_ip = lookup->ip_blacklist.data[i];

            if (strcmp(client_ip, blocked_ip) == 0) {
                http_res = xps_http_res_create(session->core, HTTP_FORBIDDEN);
                if (http_res == NULL) {
                    logger(LOG_ERROR, "session_process_request()", "xps_http_res_create() failed");
                    return;
                }

                xps_buffer_t *buff = xps_http_res_serialize(http_res);
                if (buff == NULL) {
                    logger(LOG_ERROR, "session_process_request()", "xps_http_res_serialize() failed");
                    xps_http_res_destroy(http_res);
                    return;
                }

                set_to_client_buff(session, buff);
                xps_http_res_destroy(http_res);
                return;
            }
        }
    }


    session->lookup = lookup;

    if (lookup->type == REQ_FILE_SERVE) {
        if(lookup->dir_path != NULL) {
            // If dir_path is not NULL, it means we need to serve a directory listing. We will generate an HTML page for the directory listing and send it as the response.
            xps_buffer_t *dir_html = xps_directory_browsing(lookup->dir_path, session->http_req->pathname);

            // Directory listing generation failed
            if(dir_html == NULL){
                http_res = xps_http_res_create(session->core, HTTP_INTERNAL_SERVER_ERROR);
                if (http_res == NULL) {
                    logger(LOG_ERROR, "session_process_request()", "xps_http_res_create() failed");
                    return;
                }
                xps_buffer_t *buff = xps_http_res_serialize(http_res);
                if (buff == NULL) {
                    logger(LOG_ERROR, "session_process_request()", "xps_http_res_serialize() failed");
                    xps_http_res_destroy(http_res);
                    return;
                }
                set_to_client_buff(session, buff);
                xps_http_res_destroy(http_res);
                return;
            }

            // create http response with 200 OK and set the body to dir_html
            http_res = xps_http_res_create(session->core, HTTP_OK);
            if (http_res == NULL) {
                logger(LOG_ERROR, "session_process_request()", "xps_http_res_create() failed");
                xps_buffer_destroy(dir_html);
                return;
            }
            xps_http_res_set_body(http_res, dir_html);

            xps_http_set_header(
                &http_res->headers,
                "Content-Type",
                "text/html"
            );

            // Set Content-Length header
            char len_str[32];
            sprintf(len_str, "%zu", dir_html->len);
            xps_http_set_header(&http_res->headers, "Content-Length", len_str);

            xps_buffer_t *buff = xps_http_res_serialize(http_res);
            if(buff == NULL) {
                logger(LOG_ERROR, "session_process_request()", "xps_http_res_serialize() failed");
                xps_http_res_destroy(http_res);
                return;
            }

            set_to_client_buff(session, buff);
            xps_http_res_destroy(http_res);
            return;
        }



        int error;
        /* create file for above path and attach to file field of session */
        logger(LOG_DEBUG, "session_process_request()", "file_path=%s", lookup->file_path);
        xps_file_t *file = xps_file_create(session->core, lookup->file_path, &error);
        session->file = file;

        // printf("[Experiment] Serving file: %s | Size: %zu bytes\n", lookup->file_path, session->file->size);

        /* handle all possible errors on file creation */
        if (error == E_PERMISSION) {
            http_res = xps_http_res_create(session->core, HTTP_FORBIDDEN);
            if (http_res == NULL) {
                logger(LOG_ERROR, "session_process_request()", "xps_http_res_create() failed");
                return;
            }
            xps_buffer_t *buff = xps_http_res_serialize(http_res);
            if (buff == NULL) {
                logger(LOG_ERROR, "session_process_request()", "xps_http_res_serialize() failed");
                xps_http_res_destroy(http_res);
                return;
            }
            set_to_client_buff(session, buff);
            xps_http_res_destroy(http_res);
            return;
        }
        else if (error == E_NOTFOUND) {
            http_res = xps_http_res_create(session->core, HTTP_NOT_FOUND);
            if (http_res == NULL) {
                logger(LOG_ERROR, "session_process_request()", "xps_http_res_create() failed");
                return;
            }
            xps_buffer_t *buff = xps_http_res_serialize(http_res);
            if (buff == NULL) {
                logger(LOG_ERROR, "session_process_request()", "xps_http_res_serialize() failed");
                xps_http_res_destroy(http_res);
                return;
            }
            set_to_client_buff(session, buff);
            xps_http_res_destroy(http_res);
            return;
        }
        else if (error != OK) {
            http_res = xps_http_res_create(session->core, HTTP_INTERNAL_SERVER_ERROR);
            if (http_res == NULL) {
                logger(LOG_ERROR, "session_process_request()", "xps_http_res_create() failed");
                return;
            }
            xps_buffer_t *buff = xps_http_res_serialize(http_res);
            if (buff == NULL) {
                logger(LOG_ERROR, "session_process_request()", "xps_http_res_serialize() failed");
                xps_http_res_destroy(http_res);
                return;
            }
            set_to_client_buff(session, buff);
            xps_http_res_destroy(http_res);
            return;
        }
        else {
            http_res = xps_http_res_create(session->core, HTTP_OK);
            logger(LOG_DEBUG, "session_process_request()", "created http response for request");
            if (http_res == NULL) {
                logger(LOG_ERROR, "session_process_request()", "xps_http_res_create() failed");
                return;
            }
            if (session->file->mime_type) {
                xps_http_set_header( &http_res->headers, "Content-Type", session->file->mime_type);
            }

            // Set Content-Length header
            char len_str[32];
            sprintf(len_str, "%zu", session->file->size);
            xps_http_set_header( &http_res->headers, "Content-Length", len_str);

            xps_buffer_t *buff = xps_http_res_serialize(http_res);
            if (buff == NULL) {
                logger(LOG_ERROR, "session_process_request()", "xps_http_res_serialize() failed");
                xps_http_res_destroy(http_res);
                return;
            }
            set_to_client_buff(session, buff);
            xps_http_res_destroy(http_res);
        }

        /* create pipe with session->file->source and session->file_sink */
        if (xps_pipe_create(session->core, DEFAULT_PIPE_BUFF_THRESH, session->file->source, session->file_sink) == NULL) {
            logger(LOG_ERROR, "session_process_request()", "xps_pipe_create() failed for file source and file sink");
            return;
        }
    }
    else if(lookup->type == REQ_REVERSE_PROXY) {
        char host[256];
        u_int port;

        sscanf(lookup->upstream, "%[^:]:%u", host, &port);

        xps_connection_t *upstream = xps_upstream_create(session->core, host, port);
        if (upstream == NULL) {
            logger(LOG_ERROR, "session_process_request()", "xps_connection_create() failed for upstream");
            http_res = xps_http_res_create(session->core, HTTP_BAD_GATEWAY);
            set_to_client_buff(session, xps_http_res_serialize(http_res));
            xps_http_res_destroy(http_res);
            return;
        }

        session->upstream = upstream;

        xps_pipe_create(session->core, DEFAULT_PIPE_BUFF_THRESH, session->upstream_source, upstream->sink);
        xps_pipe_create(session->core, DEFAULT_PIPE_BUFF_THRESH, upstream->source, session->upstream_sink);
    }
    else if(lookup->type == REQ_REDIRECT) {
        http_res = xps_http_res_create(session->core, lookup->http_status_code);
        xps_http_set_header( &http_res->headers, "Location", lookup->redirect_url);
        xps_buffer_t *buff = xps_http_res_serialize(http_res);
        /*serialize the reponse*/
        set_to_client_buff(session, buff);
        /* destroy the response object after use */
        xps_http_res_destroy(http_res);
        return;
    }
    else {
        logger(LOG_ERROR, "session_process_request()", "invalid lookup type");
        /*destroy session*/
        xps_session_destroy(session);
        return;
    }
}