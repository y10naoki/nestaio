/* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 * The MIT License
 *
 * Copyright (c) 2010-2011 YAMAMOTO Naoki
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#ifndef _NIO_SERVER_H_
#define _NIO_SERVER_H_

#include "nestalib.h"

#define PROGRAM_NAME        "nestaio"
#define PROGRAM_VERSION     "0.3.1"

#define DEFAULT_PORT            11211   /* memcached listen port */
#define DEFAULT_BACKLOG         100     /* listen backlog number */
#define DEFAULT_WORKER_THREADS  4       /* worker threads number */
#define DEFAULT_BUCKET_NUM      1000000 /* hash bucket size */

#define STATUS_CMD          "__/status/__"
#define SHUTDOWN_CMD        "__/shutdown/__"

/* thread argument */
struct thread_args_t {
    SOCKET socket;
    struct sockaddr_in sockaddr;
};

/* program configuration */
struct nio_conf_t {
    int daemonize;                      /* execute as daemon(Linux/MacOSX only) */
    char username[256];                 /* execute as username(Linux/MacOSX only) */
    ushort port_no;                     /* listen port number */
    int backlog;                        /* listen backlog number */
    int worker_threads;                 /* worker thread number */
    char nio_path[MAX_PATH+1];          /* nestaIO database file path */
    struct nio_t* nio_db;               /* nestaIO database object */
    int nio_bucket_num;                 /* nestaIO bucket number */
    int nio_mmap_size;                  /* nestaIO mmap size(MB) */
    char error_file[MAX_PATH+1];        /* error file name */
    char output_file[MAX_PATH+1];       /* output file name */
};

/* macros */
#define TRACE(fmt, ...) \
    if (g_trace_mode) { \
        fprintf(stdout, fmt, __VA_ARGS__); \
    }

#ifdef _WIN32
#define get_abspath(abs_path, path, maxlen) \
    _fullpath(abs_path, path, maxlen)
#else
#define get_abspath(abs_path, path, maxlen) \
    realpath(path, abs_path)
#endif

/* global variables */
#ifndef _MAIN
    extern
#endif
struct nio_conf_t* g_conf;  /* read only configure data */

#ifndef _MAIN
    extern
#endif
SOCKET g_listen_socket;     /* listen socket */

#ifndef _MAIN
    extern
#endif
int g_shutdown_flag;        /* not zero is shutdown mode */

#ifndef _MAIN
    extern
#endif
int g_trace_mode;           /* not zero is trace mode */

#ifndef _MAIN
    extern
#endif
struct queue_t* g_queue;    /* request queue */

#ifndef _MAIN
    extern
#endif
int64 g_start_time;         /* start time of server */


#ifndef _MAIN
    extern
#endif
void* g_sock_event;         /* socket event */

#ifndef _MAIN
    extern
#endif
struct hash_t* g_sockbuf_hash;

/* prototypes */
#ifdef __cplusplus
extern "C" {
#endif

/* nio_config.c */
int config(const char* conf_fname);

/* nio_server.c */
void nio_server();

/* nio_command.c */
void stop_server();
void status_server();

/* memcached.c */
int memcached_request(SOCKET socket, struct sockaddr_in sockaddr);
int memcached_worker_open();
int memcached_open();
void memcached_close();

#ifdef __cplusplus
}
#endif

#endif  /* _NIO_SERVER_H_ */
