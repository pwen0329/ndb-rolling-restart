/*
 * ndb_rolling_restart.hpp
 * Copyright (C) 2018 Eric Herman <eric@freesa.org>
 *
 * This work is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This work is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 */

#ifndef NDB_ROLLING_RESTART_H
#define NDB_ROLLING_RESTART_H 1

#include <mgmapi/mgmapi.h> // typedef struct ndb_mgm_handle * NdbMgmHandle;
#include <ndbapi/NdbApi.hpp> // class Ndb_cluster_connection

#define NDB_NORMAL_USER 0

struct ndb_connection_context_s {
    const char* connect_string;
    unsigned wait_seconds;
    unsigned wait_after_restart;
    Ndb_cluster_connection* connection;
    NdbMgmHandle ndb_mgm_handle; /* a ptr */
    struct ndb_mgm_cluster_state* cluster_state;
};

struct restart_node_status_s {
    int node_group;
    int node_id;
    bool was_restarted;
};

void close_ndb_connection(struct ndb_connection_context_s* ndb_ctx);

int init_ndb_connection(struct ndb_connection_context_s* ndb_ctx);

int restart_node(struct ndb_connection_context_s* ndb_ctx, int node_id);

void sort_node_restarts(struct restart_node_status_s* node_restarts,
    size_t number_of_nodes);

void get_node_restarts(struct ndb_mgm_cluster_state* cluster_state,
    struct restart_node_status_s* node_restarts, size_t number_of_nodes);

void report_cluster_state(struct ndb_connection_context_s* ndb_ctx);

#endif /* BINARY_SEARCH_H */
