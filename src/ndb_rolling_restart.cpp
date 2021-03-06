/*
 * ndb_rolling_restart
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

#include "ndb_rolling_restart.hpp"
#include "binary_search.h"
#include <assert.h>
#include <iostream>
#include <string.h>
#include <unistd.h>

using namespace std;

#define Cerr cerr << __FILE__ << ":" << __LINE__ << ": "

void close_ndb_connection(struct ndb_connection_context_s* ndb_ctx)
{
    assert(ndb_ctx);

    if (ndb_ctx->cluster_state) {
        free((void*)ndb_ctx->cluster_state);
        ndb_ctx->cluster_state = nullptr;
    }

    if (ndb_ctx->ndb_mgm_handle) {
        ndb_mgm_destroy_handle(&(ndb_ctx->ndb_mgm_handle));
        ndb_ctx->ndb_mgm_handle = nullptr;
    }

    if (ndb_ctx->connection) {
        delete (ndb_ctx->connection);
        ndb_ctx->connection = nullptr;
    }
}

static Ndb_cluster_connection* ndb_connect(const char* connect_string,
    unsigned wait_seconds)
{
    Ndb_cluster_connection* cluster_connection;

    cluster_connection = new Ndb_cluster_connection(connect_string);
    if (!cluster_connection) {
        Cerr << "new Ndb_cluster_connection() returned 0" << endl;
        return nullptr;
    }

    int no_retries = 10;
    int retry_delay_in_seconds = wait_seconds;
    int verbose = 1;
    cluster_connection->connect(no_retries, retry_delay_in_seconds, verbose);

    int before_wait = wait_seconds;
    int after_wait = wait_seconds;
    if (cluster_connection->wait_until_ready(before_wait, after_wait) < 0) {
        Cerr << "Cluster was not ready within "
             << wait_seconds << " seconds" << endl;
        delete (cluster_connection);
        return nullptr;
    }
    return cluster_connection;
}

int init_ndb_connection(struct ndb_connection_context_s* ndb_ctx)
{
    assert(ndb_ctx);

    ndb_ctx->connection = ndb_connect(ndb_ctx->connect_string,
        ndb_ctx->wait_seconds);
    if (!ndb_ctx->connection) {
        return 1;
    }

    ndb_ctx->ndb_mgm_handle = ndb_mgm_create_handle();
    if (!ndb_ctx->ndb_mgm_handle) {
        Cerr << "Error: ndb_mgm_create_handle returned null?" << endl;
        close_ndb_connection(ndb_ctx);
        return 1;
    }

    int no_retries = 10;
    int retry_delay_secs = 3;
    int verbose = 1;

    int ret = ndb_mgm_connect(ndb_ctx->ndb_mgm_handle, no_retries,
        retry_delay_secs, verbose);

    if (ret != 0) {
        Cerr "ndb_mgm_get_latest_error: "
            << ndb_mgm_get_latest_error_msg(ndb_ctx->ndb_mgm_handle)
            << endl;
        close_ndb_connection(ndb_ctx);
        return 1;
    }

    ndb_mgm_node_type node_types[2] = {
        /* NDB_MGM_NODE_TYPE_MGM, */ /* SKIP management server node */
        NDB_MGM_NODE_TYPE_NDB, /* database node, what we actually want */
        NDB_MGM_NODE_TYPE_UNKNOWN /* weird */
    };

    ndb_ctx->cluster_state = ndb_mgm_get_status2(ndb_ctx->ndb_mgm_handle,
        node_types);

    if (!ndb_ctx->cluster_state) {
        Cerr << "ndb_mgm_get_status2 returned null?" << endl;
        close_ndb_connection(ndb_ctx);
        return 1;
    }

    return 0;
}

static const string get_ndb_mgm_dump_state(NdbMgmHandle ndb_mgm_handle,
    struct ndb_mgm_node_state* node_state)
{
    assert(ndb_mgm_handle);
    assert(node_state);

    int arg_count = 1;
    int args[1] = { 1000 };
    struct ndb_mgm_reply reply;
    reply.return_code = 0;
    int node_id = node_state->node_id;
    int rv;

    rv = ndb_mgm_dump_state(ndb_mgm_handle, node_id, args, arg_count, &reply);
    if (rv == -1) {
        return "error: Could not dump state";
    }

    if (reply.return_code != 0) {
        return reply.message;
    }

    return "ok";
}

static int get_online_node_count(struct ndb_mgm_cluster_state* cluster_state)
{
    int online_nodes = 0;

    assert(cluster_state);
    for (int i = 0; i < cluster_state->no_of_nodes; ++i) {
        struct ndb_mgm_node_state* node_state;
        node_state = &(cluster_state->node_states[i]);
        if (node_state->node_status == NDB_MGM_NODE_STATUS_STARTED) {
            ++online_nodes;
        }
    }

    return online_nodes;
}

static void sleep_reconnect(struct ndb_connection_context_s* ndb_ctx)
{
    close_ndb_connection(ndb_ctx);
    cout << "sleep(" << ndb_ctx->wait_seconds << ")" << endl;
    sleep(ndb_ctx->wait_seconds);
    int err = init_ndb_connection(ndb_ctx);
    if (err) {
        Cerr << "could not reconnect to ndb" << endl;
    }
}

static int loop_wait_until_ready(struct ndb_connection_context_s* ndb_ctx,
    int node_id)
{

    assert(ndb_ctx->connection);

    int cnt = 1;
    int nodes[1] = { node_id };

    int ret = -1;
    while (ret == -1) {
        cout << "wait_until_ready node " << nodes[0]
             << " timeout: " << ndb_ctx->wait_seconds << endl;
        ret = ndb_ctx->connection->wait_until_ready(nodes, cnt,
            ndb_ctx->wait_seconds);
        if (ret <= -1) {
            Cerr << "ndb_mgm_restart4 returned error: " << ret << endl;
            sleep_reconnect(ndb_ctx);
        }
    }
    return 0;
}

int restart_node(struct ndb_connection_context_s* ndb_ctx, int node_id)
{
    assert(ndb_ctx);

    int ret = 0;
    int disconnect = 0;
    int cnt = 1;
    int nodes[1] = { node_id };
    int initial = 0;
    int nostart = 0;
    int abort = 0;
    int force = 0;

    cout << "ndb_mgm_restart4 node " << nodes[0] << endl;

    loop_wait_until_ready(ndb_ctx, nodes[0]);

    ret = -1;
    while (ret <= 0) {
        ret = ndb_mgm_restart4(ndb_ctx->ndb_mgm_handle, cnt, nodes, initial,
            nostart, abort, force, &disconnect);
        if (ret <= 0) {
            cout << __FILE__ << ":" << __LINE__
                 << ": ndb_mgm_restart4 node " << nodes[0]
                 << " returned error: " << ret << endl;
            sleep_reconnect(ndb_ctx);
        }
    }

    if (disconnect) {
        sleep_reconnect(ndb_ctx);
    }

    if (ndb_ctx->wait_after_restart) {
        loop_wait_until_ready(ndb_ctx, nodes[0]);
    }

    cout << "restart node " << nodes[0] << " complete" << endl;
    return 0;
}

void sort_node_restarts(struct restart_node_status_s* node_restarts,
    size_t number_of_nodes)
{
    assert(node_restarts);
    assert(number_of_nodes);

    // group them so they alternate by groups, lowest group id first

    // there can't be more groups than nodes
    int group_ids[number_of_nodes];
    size_t group_count = 0;

    // create a sorted array of group ids without duplicates
    size_t target_index;
    for (size_t i = 0; i < number_of_nodes; ++i) {
        binary_search_result search_result = binary_search_ints(group_ids,
            group_count, node_restarts[i].node_group, &target_index);
        if (search_result == binary_search_insert) {
            // we have enough space for sure, so we can move all current
            // elements over without writing past the end of the array
            // this way we end up with a sorted array (ascending)
            size_t elements_to_move = group_count - target_index;
            memmove((group_ids + target_index + 1), (group_ids + target_index),
                (sizeof(int) * elements_to_move));
            group_ids[target_index] = node_restarts[i].node_group;
            ++group_count;
        }
        // already in the array
    }

    // now "take" nodes in turn from each of the groups
    // the convenient way is to start at node 0,
    // find a node from group 0 and swap them
    // then got to node 1 and to get a node from group 1 etc,
    //     looping the groups when at the end
    // if there is no node left from a group,
    // then remove that group from the list and keep going
    size_t update_node_index = 0;
    size_t current_node_index = 0;
    size_t current_group_index = 0;
    restart_node_status_s temp;
    while (update_node_index < number_of_nodes) {

        current_node_index = update_node_index;
        while (node_restarts[current_node_index].node_group
            != group_ids[current_group_index]) {
            ++current_node_index;

            // if we reach the end without finding any,
            // then this group is exhausted.
            // remove this group from the list,
            // update the group count
            // and ensure current_group_index is within bounds
            if (current_node_index == number_of_nodes) {

                // move 1 left
                size_t elements_to_move = group_count - current_group_index - 1;

                memmove((group_ids + current_group_index),
                    (group_ids + current_group_index + 1),
                    (sizeof(int) * elements_to_move));
                --group_count;

                // in case it was at the last one, point back at 0
                current_group_index %= group_count;
                // start search over at the beginning
                current_node_index = update_node_index;
            }
        }

        temp = node_restarts[update_node_index];
        node_restarts[update_node_index] = node_restarts[current_node_index];
        node_restarts[current_node_index] = temp;

        ++update_node_index;
        current_group_index = (current_group_index + 1) % group_count;
    }
}

void get_node_restarts(struct ndb_mgm_cluster_state* cluster_state,
    struct restart_node_status_s* node_restarts, size_t number_of_nodes)
{
    assert(cluster_state);
    assert(number_of_nodes);

    for (size_t i = 0; i < number_of_nodes; ++i) {
        struct ndb_mgm_node_state* node_state;
        node_state = &(cluster_state->node_states[i]);
        node_restarts[i].node_group = node_state->node_group;
        node_restarts[i].node_id = node_state->node_id;
        node_restarts[i].was_restarted = 0;
    }
}

void report_cluster_state(struct ndb_connection_context_s* ndb_ctx)
{
    assert(ndb_ctx);
    assert(ndb_ctx->connection);
    assert(ndb_ctx->cluster_state);

    const char* cluster_name = ndb_ctx->connection->get_system_name();
    cout << "cluster_name: " << cluster_name << endl;
    cout << "cluster_state->no_of_nodes: "
         << ndb_ctx->cluster_state->no_of_nodes << endl;

    for (int i = 0; i < ndb_ctx->cluster_state->no_of_nodes; ++i) {

        struct ndb_mgm_node_state* node_state;
        node_state = &(ndb_ctx->cluster_state->node_states[i]);

        cout << "node_id: " << node_state->node_id << " ("
             << ndb_mgm_get_node_type_string(node_state->node_type) << ")"
             << endl
             << "\tstatus: " << node_state->node_status << " ("
             << ndb_mgm_get_node_status_string(node_state->node_status)
             << ")" << endl;

        if ((node_state->node_type == NDB_MGM_NODE_TYPE_NDB)
            && (node_state->node_status == NDB_MGM_NODE_STATUS_STARTING)) {
            cout << "\tstart_phase: " << node_state->start_phase
                 << endl;
        }

        cout << "\tdynamic_id: " << node_state->dynamic_id << endl
             << "\tnode_group: " << node_state->node_group << endl
             << "\tversion: " << node_state->version << endl
             << "\tmysql_version: " << node_state->mysql_version << endl
             << "\tconnect_count: " << node_state->connect_count << endl
             << "\tconnect_address: " << node_state->connect_address << endl
             << "\tndb_mgm_dump_state: "
             << get_ndb_mgm_dump_state(ndb_ctx->ndb_mgm_handle, node_state)
             << endl;
    }

    int online_nodes = get_online_node_count(ndb_ctx->cluster_state);

    int offline_nodes = (ndb_ctx->cluster_state->no_of_nodes - online_nodes);

    cout << "no_of_nodes: " << ndb_ctx->cluster_state->no_of_nodes << endl
         << "online_nodes: " << online_nodes << endl
         << "offline_nodes: " << offline_nodes << endl;
}

int ndb_rolling_restart(struct ndb_connection_context_s* ndb_ctx)
{
    ndb_init();

    int err = init_ndb_connection(ndb_ctx);
    if (err) {
        Cerr << "error connecting to ndb '" << ndb_ctx->connect_string << "'"
             << endl;
        return 1;
    }

    report_cluster_state(ndb_ctx);

    if (ndb_ctx->cluster_state->no_of_nodes < 1) {
        Cerr << "cluster_state->no_of_nodes == "
             << ndb_ctx->cluster_state->no_of_nodes << " ?" << endl;
        close_ndb_connection(ndb_ctx);
        return EXIT_FAILURE;
    }
    size_t number_of_nodes = (size_t)ndb_ctx->cluster_state->no_of_nodes;

    struct restart_node_status_s node_restarts[number_of_nodes];
    get_node_restarts(ndb_ctx->cluster_state, node_restarts, number_of_nodes);

    if (false) {
        // Testing suggests a possible bug here.
        // For now, rather than sort, we'll rely upon
        // the last_group check while looping over groups
        sort_node_restarts(node_restarts, number_of_nodes);
    }

    unsigned restarted = 0;
    int last_group = -1;
    for (size_t i = 0; restarted < number_of_nodes; ++i) {
        if (i >= number_of_nodes) {
            i = 0;
            last_group = -1;
        }
        if ((node_restarts[i].node_group != last_group)
            && !node_restarts[i].was_restarted) {
            ++restarted;
            node_restarts[i].was_restarted = 1;
            last_group = node_restarts[i].node_group;
            restart_node(ndb_ctx, node_restarts[i].node_id);
        }
    }

    report_cluster_state(ndb_ctx);

    close_ndb_connection(ndb_ctx);
    ndb_end(NDB_NORMAL_USER);
    return 0;
}
