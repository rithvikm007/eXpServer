#include "xps_config.h"

xps_config_listener_t *parse_listener(JSON_Object *listener_object) ;
xps_config_route_t *parse_route(JSON_Object *route_object);
void parse_server(JSON_Object *server_object, xps_config_server_t *server);
void parse_all_listeners(xps_config_t *config);

xps_config_t *xps_config_create(const char *config_path) {
    /*assert*/
    assert(config_path != NULL);
    /*allocate mem for config*/
    xps_config_t *config = malloc(sizeof(xps_config_t));
    if (config == NULL) {
        logger(LOG_ERROR, "xps_config_create()", "Failed to allocate memory for config");
        return NULL;
    }
    /*get config_json using json_parse_file*/
    JSON_Value *config_json = json_parse_file(config_path);
    if (config_json == NULL) {
        logger(LOG_ERROR, "xps_config_create()", "Failed to parse config file: %s", config_path);
        free(config);
        return NULL;
    }
    /*initialize fields of config object*/
    config->config_path = config_path;
    vec_init(&config->servers);
    vec_init(&config->_all_listeners);
    config->_config_json = config_json;
    JSON_Object *root_object = json_value_get_object(config_json);
    /*initialize server_name,servers fields - hint: use json_object_get_string
    ,json_object_get_number,json_object_get_array*/
    config->server_name = json_object_get_string(root_object, "server_name");
    JSON_Array *servers = json_object_get_array(root_object, "servers");
    for (size_t i = 0; i < json_array_get_count(servers); i++) {
        xps_config_server_t *server = malloc(sizeof(xps_config_server_t));
        if (server == NULL) {
            logger(LOG_ERROR, "xps_config_create()", "Failed to allocate memory for server");
            json_value_free(config_json);
            free(config);
            return NULL;
        }
        //initialize and parse the server and push to servers list of config
        parse_server(json_array_get_object(servers, i), server);
        vec_push(&config->servers, server);
    }
    /*initialize and set up the _all_listeners array*/
    parse_all_listeners(config);
    return config;
}

void xps_config_destroy(xps_config_t *config) {
    assert(config != NULL);
    // free the servers array
    vec_void_t *servers = &config->servers;
    for(int i = 0; i < servers->length; i++) {
        xps_config_server_t *server = (xps_config_server_t *)servers->data[i];
        if(server) {
            // free the listeners array
            vec_void_t *listeners = &server->listeners;
            for(int j = 0; j < listeners->length; j++) {
                xps_config_listener_t *listener = (xps_config_listener_t *)listeners->data[j];
                if(listener) free(listener);
            }
            vec_deinit(&server->listeners);
            // free the hostnames array
            vec_deinit(&server->hostnames);
            // free the routes array
            vec_void_t *routes = &server->routes;
            for(int j = 0; j < routes->length; j++) {
                xps_config_route_t *route = (xps_config_route_t *)routes->data[j];
                if(route) {
                    vec_deinit(&route->index);
                    vec_deinit(&route->upstreams);
                    free(route);
                }
            }
            vec_deinit(&server->routes);
            free(server);
        }
    }
}

xps_config_lookup_t *xps_config_lookup(xps_config_t *config, xps_http_req_t *http_req, xps_connection_t *client, int *error) {
    /*assert*/
    assert(config != NULL);
    assert(http_req != NULL);
    assert(client != NULL);
    *error = E_FAIL;
    /*get host,accept encoding,pathname from http_req*/
    char* host = http_req->host;
    char* pathname = http_req->pathname;
    const char* accept_encoding = xps_http_get_header(&http_req->headers, "Accept-Encoding");
    // Step 1: Find matching server block
    int target_server_index = -1;
    vec_void_t *servers = &config->servers;
    for (int i = 0; i < servers->length; i++) {
        xps_config_server_t *server = servers->data[i];
        // Check if client listener is present in server
        vec_void_t *listeners = &server->listeners;
        int has_matching_listener = false;
        for (int j = 0; j < listeners->length; j++) {
            /*fill this*/
            xps_config_listener_t *listener = listeners->data[j];
            if (strcmp(listener->host, client->listener->host) == 0 && listener->port == client->listener->port) {
                has_matching_listener = true;
                break;
            }
        }
        if (!has_matching_listener) continue;
        /* Check if host header matches any hostname*/
        vec_void_t *hostnames = &server->hostnames;
        int has_matching_hostname = false;
        // NOTE: if not hostnames provided, it is considered a match
        if(hostnames->length == 0) {
            has_matching_hostname = true;
        } else {
            for (int j = 0; j < hostnames->length; j++) {
                const char *hostname = hostnames->data[j];
                if (strcmp(hostname, host) == 0) {
                    has_matching_hostname = true;
                    break;
                }
            }
        }
        if (has_matching_hostname) {
            target_server_index = i;
            break;
        }
    }

    if(target_server_index == -1) {
        logger(LOG_ERROR, "xps_config_lookup()", "No matching server found for host: %s, listener: %s:%u", host, client->listener->host, client->listener->port);
        *error = E_NOTFOUND;  // No matching server found - 404
        return NULL;
    }

    xps_config_server_t *server = config->servers.data[target_server_index];

    /*Find matching route block*/
    // Route matching uses prefix matching with longest-match-first strategy.
    // This is important because:
    // - For file serving routes (e.g., "/"), we need to match any path under it
    //   (e.g., "/index.html", "/css/style.css" should all match route "/")
    // - For specific routes (e.g., "/api"), we want them to take precedence over "/"
    //
    // Example: If we have routes "/" and "/api"
    // - Request "/index.html" matches "/" only → serves file from "/"
    // - Request "/api/users" matches both "/" and "/api" → use "/api" (longest match)

    xps_config_route_t *route = NULL;
    size_t best_match_len = 0;  // Track the longest matching route path

    for (int i = 0; i < server->routes.length; i++) {
        xps_config_route_t *current_route = server->routes.data[i];
        size_t route_path_len = strlen(current_route->req_path);

        // Check if this route's path is a prefix of the request path
        if (str_starts_with(pathname, current_route->req_path)) {
            // If this is a longer match than we've found so far, use it
            if (route_path_len > best_match_len) {
                /*fill the rest*/
                best_match_len = route_path_len;
                route = current_route;
            }
        }
    }

    if (route == NULL) {
        *error = E_NOTFOUND;  // No matching route found - 404
        return NULL;
    }
    /* Init values of lookup*/
    xps_config_lookup_t *lookup = malloc(sizeof(xps_config_lookup_t));
    if (lookup == NULL) {
        logger(LOG_ERROR, "xps_config_lookup()", "Failed to allocate memory for lookup");
        *error = E_FAIL;
        return NULL;
    }
    lookup->ip_whitelist = route->ip_whitelist;
    lookup->ip_blacklist = route->ip_blacklist;
    lookup->file_path = NULL;
    lookup->dir_path = NULL;
    lookup->upstream = NULL;
    lookup->http_status_code = 0;
    lookup->redirect_url = NULL;

    if(strcmp(route->type, "file_serve") == 0) {
        lookup->type = REQ_FILE_SERVE;
    } else if(strcmp(route->type, "reverse_proxy") == 0) {
        lookup->type = REQ_REVERSE_PROXY;
    } else if(strcmp(route->type, "redirect") == 0) {
        lookup->type = REQ_REDIRECT;
    } else if(strcmp(route->type, "metrics") == 0) {
        lookup->type = REQ_METRICS;
    } else {
        lookup->type = REQ_INVALID;
    }

    // File serve
    if (lookup->type == REQ_FILE_SERVE) {
        char *resource_path = path_join(route->dir_path, pathname);

        if (resource_path == NULL) {
            logger(LOG_WARNING, "xps_config_lookup()", "Failed to construct resource path");
            *error = E_FAIL;
            free(lookup);
            return NULL;
        }

        if (!is_abs_path(resource_path)) {
            /* we require abosulte path so we need to see
            if the current path is absolute or not */
            /*fill here*/
            char *abs_path = realpath(resource_path, NULL);
            free(resource_path);
            resource_path = abs_path;
        }
        if(resource_path == NULL) {
            logger(LOG_WARNING, "xps_config_lookup()", "Resource not found: %s", pathname);
            *error = E_NOTFOUND;
            free(lookup);
            return NULL;
        }
        // is file
        if (is_file(resource_path)) {
            lookup->file_path = resource_path;
        } 
        else if (is_dir(resource_path)) { // is directory
            /* If request is for a directory, serve the index file (e.g. index.html)
            * instead of showing the directory listing. */
            bool index_file_found = false;
            for (int i = 0; i < route->index.length; i++) {
                char *index_file = path_join(resource_path, route->index.data[i]);
                /*fill here*/
                if(is_file(index_file)) {
                    lookup->file_path = index_file;
                    index_file_found = true;
                    break;
                } 
                else free(index_file);   
            }
            if (!index_file_found) {
                // If no index file is found, we can serve the directory listing page.
                lookup->dir_path = resource_path;
            }
        } 
        else {
            /*no matching type so free resource_path*/
            free(resource_path);
            *error = E_NOTFOUND;
            free(lookup);
            return NULL;
        }
        *error = OK;
        return lookup;
    }
    if (lookup->type == REQ_REVERSE_PROXY) {
        // For reverse proxy, we can just take the first upstream from the list of upstreams
        lookup->upstream = route->upstreams.data[0];
        *error = OK;
        return lookup;
    }

    if (lookup->type == REQ_REDIRECT) {
        // For redirect, we can just take the http_status_code and redirect_url from the route
        lookup->http_status_code = route->http_status_code;
        lookup->redirect_url = route->redirect_url;
        *error = OK;
        return lookup;
    }

    free(lookup);
    *error = E_FAIL;
    return NULL;
}

void xps_config_lookup_destroy(xps_config_lookup_t *lookup, xps_core_t *core) {
    // assert
    assert(lookup != NULL);
    assert(core != NULL);
    
    // free the dynamically allocated fields in lookup
    if(lookup->file_path) free(lookup->file_path);
    if(lookup->dir_path) free(lookup->dir_path);

    // Note that we do not free the ip_whitelist and ip_blacklist vectors here, as they are owned by the route and will be freed when the config is destroyed.

    free(lookup);
}

xps_config_listener_t *parse_listener(JSON_Object *listener_object) {
    assert(listener_object != NULL);

    // Parse the listener object to extract host and port
    const char *host = json_object_get_string(listener_object, "host");
    u_int port = (u_int)json_object_get_number(listener_object, "port");

    xps_config_listener_t *listener = malloc(sizeof(xps_config_listener_t));
    if (listener == NULL) {
        logger(LOG_ERROR, "parse_listener()", "Failed to allocate memory for listener");
        return NULL;
    }

    listener->host = host;
    listener->port = port;

    return listener;
}

xps_config_route_t *parse_route(JSON_Object *route_object) {
    // assert
    assert(route_object != NULL);

    xps_config_route_t *route = malloc(sizeof(xps_config_route_t));
    if (route == NULL) {
        logger(LOG_ERROR, "parse_route()", "Failed to allocate memory for route");
        return NULL;
    }

    // Initialize the index and upstreams vectors
    vec_init(&route->index);
    vec_init(&route->upstreams);

    // Initialize the ip_whitelist and ip_blacklist vectors
    vec_init(&route->ip_whitelist);
    vec_init(&route->ip_blacklist);

    // Parse the route object to extract req_path, type, dir_path, http_status_code, and redirect_url
    route->req_path = json_object_get_string(route_object, "req_path");
    route->type = json_object_get_string(route_object, "type");
    route->dir_path = NULL;
    route->http_status_code = 0;
    route->redirect_url = NULL;

    assert(route->type != NULL);
    assert(route->req_path != NULL);

    if(strcmp(route->type, "file_serve") == 0) {
        route->dir_path = json_object_get_string(route_object, "dir_path");
        assert(route->dir_path != NULL);

        JSON_Array *index = json_object_get_array(route_object, "index");
        for(size_t i = 0; i < json_array_get_count(index); i++) {
            vec_push(&route->index, (void*)json_array_get_string(index, i));
        }
    }
    else if (strcmp(route->type, "reverse_proxy") == 0) {
        JSON_Array *upstreams = json_object_get_array(route_object, "upstreams");
        assert(upstreams != NULL);
        for(size_t i = 0; i < json_array_get_count(upstreams); i++) {
            vec_push(&route->upstreams, (void*)json_array_get_string(upstreams, i));
        }
    }
    else if (strcmp(route->type, "redirect") == 0) {
        route->http_status_code = (u_int)json_object_get_number(route_object, "http_status_code");
        route->redirect_url = json_object_get_string(route_object, "redirect_url");

        assert(route->http_status_code != 0);
        assert(route->redirect_url != NULL);
    }

    // Parse the ip_whitelist and ip_blacklist arrays
    JSON_Array *ip_whitelist = json_object_get_array(route_object, "ip_whitelist");
    JSON_Array *ip_blacklist = json_object_get_array(route_object, "ip_blacklist");

    // If ip_whitelist exists but ip_blacklist does not, populate the ip_whitelist vector
    if (ip_whitelist && !ip_blacklist) {
        for (size_t i = 0; i < json_array_get_count(ip_whitelist); i++) {
            const char *ip =
                json_array_get_string(ip_whitelist, i);

            vec_push(&route->ip_whitelist, (void *)ip);
        }
    }
    // If ip_blacklist exists but ip_whitelist does not, populate the ip_blacklist vector
    else if (ip_blacklist && !ip_whitelist) {
        for (size_t i = 0; i < json_array_get_count(ip_blacklist); i++) {
            const char *ip =
                json_array_get_string(ip_blacklist, i);

            vec_push(&route->ip_blacklist, (void *)ip);
        }
    }
    // If both ip_whitelist and ip_blacklist exist, populate the ip_blacklist vector first,
    // then populate the ip_whitelist vector while ensuring that any IPs in the ip_blacklist are not added to the ip_whitelist vector.
    else if (ip_whitelist && ip_blacklist) {
        for (size_t i = 0;
            i < json_array_get_count(ip_blacklist);
            i++) {

            const char *ip =
                json_array_get_string(ip_blacklist, i);

            vec_push(&route->ip_blacklist, (void *)ip);
        }

        for (size_t i = 0;
            i < json_array_get_count(ip_whitelist);
            i++) {

            const char *ip =
                json_array_get_string(ip_whitelist, i);

            bool blacklisted = false;

            for (size_t j = 0;
                j < route->ip_blacklist.length;
                j++) {

                const char *blocked_ip =
                    route->ip_blacklist.data[j];

                if (strcmp(ip, blocked_ip) == 0) {
                    blacklisted = true;
                    break;
                }
            }

            if (!blacklisted) {
                vec_push(&route->ip_whitelist, (void *)ip);
            }
        }
    }



    return route;
}

void parse_server(JSON_Object *server_object, xps_config_server_t *server) {
    // assert
    assert(server_object != NULL);
    assert(server != NULL);

    // Initialize the listeners, hostnames, and routes vectors
    vec_init(&server->listeners);
    vec_init(&server->hostnames);
    vec_init(&server->routes);

    // Parse the server object to extract listeners, hostnames, and routes
    JSON_Array *listeners = json_object_get_array(server_object, "listeners");
    for(size_t i = 0; i < json_array_get_count(listeners); i++) {
        xps_config_listener_t *listener = parse_listener(json_array_get_object(listeners, i));
        if(listener) vec_push(&server->listeners, listener);
    }

    JSON_Array *hostnames = json_object_get_array(server_object, "hostnames");
    for(size_t i = 0; i < json_array_get_count(hostnames); i++) {
        const char *hostname = json_array_get_string(hostnames, i);
        if(hostname) vec_push(&server->hostnames, (void*)hostname);
    }

    JSON_Array *routes = json_object_get_array(server_object, "routes");
    for(size_t i = 0; i < json_array_get_count(routes); i++) {
        xps_config_route_t *route = parse_route(json_array_get_object(routes, i));
        if(route) vec_push(&server->routes, route);
    }
}

void parse_all_listeners(xps_config_t *config) {
    // assert
    assert(config != NULL);

    // Iterate through all servers and their listeners, and add unique listeners to the _all_listeners vector
    for(int i = 0; i < config->servers.length; i++) {
        xps_config_server_t *server = config->servers.data[i];
        for(int j = 0; j < server->listeners.length; j++) {
            xps_config_listener_t *listener = server->listeners.data[j];
            
            bool already_exists = false;
            for(int k = 0; k < config->_all_listeners.length; k++) {
                xps_config_listener_t *existing_listener = config->_all_listeners.data[k];
                if(strcmp(existing_listener->host, listener->host) == 0 && existing_listener->port == listener->port) {
                    already_exists = true;
                    break;
                }
            }

            if(!already_exists) vec_push(&config->_all_listeners, listener);
        }
    }
}