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
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#define _MAIN
#include "nio_server.h"

#ifndef WIN32
#include <pwd.h>
#endif

#define DEFAULT_CONF_FILE  "./conf/" PROGRAM_NAME ".conf"

#define ACT_START  0
#define ACT_STOP   1
#define ACT_STATUS 2

static char* conf_file = NULL;  /* config file name */
static int action = ACT_START;  /* ACT_START, ACT_STOP, ACT_STATUS */

static int shutdown_done_flag = 0;  /* shutdown済みフラグ */
static int cleanup_done_flag = 0;   /* cleanup済みフラグ */

static CS_DEF(shutdown_lock);
static CS_DEF(cleanup_lock);

static void version()
{
    fprintf(stdout, "%s/%s\n", PROGRAM_NAME, PROGRAM_VERSION);
    fprintf(stdout, "Copyright (c) 2010-2011 YAMAMOTO Naoki\n\n");
}

static void usage()
{
    version();
    fprintf(stdout, "\nusage: %s [-start | -stop | -version] [-f conf.file]\n\n", PROGRAM_NAME);
}

static void cleanup()
{
    /* Ctrl-Cで終了させたときに Windowsでは nio_server()の
       メインループでも割り込みが起きて後処理のcleanup()が
       呼ばれるため、１回だけ実行されるように制御します。*/
    CS_START(&cleanup_lock);
    if (! cleanup_done_flag) {
        if (g_listen_socket != INVALID_SOCKET) {
            shutdown(g_listen_socket, 2);  /* 2: RDWR stop */
            SOCKET_CLOSE(g_listen_socket);
        }

        if (action == ACT_START) {
            if (g_queue != NULL) {
                que_finalize(g_queue);
                TRACE("%s terminated.\n", "event queue");
            }
        }
        logout_finalize();
        err_finalize();
        sock_finalize();
        mt_finalize();
        cleanup_done_flag = 1;
    }
    CS_END(&cleanup_lock);
}

static void sig_handler(int signo)
{
    if (signo == SIGINT || signo == SIGTERM) {
        /* Windowsでは、メインスレッドのみで割り込みがかかるが、
           古いLinuxカーネルの pthread ではすべてのワーカースレッドに
           割り込みがかかるため、プログラムの停止を１回のみ
           実行するように制御します。*/
        CS_START(&shutdown_lock);
        if (! shutdown_done_flag) {
            cleanup();
            shutdown_done_flag = 1;
            printf("\n%s was terminated.\n", PROGRAM_NAME);
        }
        exit(0);
        CS_END(&shutdown_lock);
#ifndef _WIN32
    } else if (signo == SIGPIPE) {
        /* ignore */
#endif
    }
}

static int startup()
{
    /* グローバル変数の初期化 */
    g_listen_socket = INVALID_SOCKET;

    /* 割り込み処理用のクリティカルセクション初期化 */
    CS_INIT(&shutdown_lock);
    CS_INIT(&cleanup_lock);

    /* マルチスレッド対応関数の初期化 */
    mt_initialize();

    /* ソケット関数の初期化 */
    sock_initialize();

    /* エラーファイルの初期化 */
    err_initialize(g_conf->error_file);

    /* アウトプットファイルの初期化 */
    logout_initialize(g_conf->output_file);

    if (action == ACT_START) {
        g_queue = que_initialize();
        if (g_queue == NULL)
            return -1;
        TRACE("%s initialized.\n", "event queue");
    }

    /* 割り込みハンドラーの登録 */
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
#ifndef _WIN32
    signal(SIGPIPE, sig_handler);
#endif
    return 0;
}

static int parse(int argc, char* argv[])
{
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp("-start", argv[i]) == 0) {
            action = ACT_START;
        } else if (strcmp("-stop", argv[i]) == 0) {
            action = ACT_STOP;
        } else if (strcmp("-status", argv[i]) == 0) {
            action = ACT_STATUS;
        } else if (strcmp("-version", argv[i]) == 0 ||
                   strcmp("--version", argv[i]) == 0) {
            version();
            return 1;
        } else if (strcmp("-f", argv[i]) == 0) {
            if (++i < argc)
                conf_file = argv[i];
            else {
                fprintf(stdout, "no config file.\n");
                return -1;
            }
        } else {
            return -1;
        }
    }
    return 0;
}

static int parse_config()
{
    /* コンフィグ領域の確保 */
    g_conf = (struct nio_conf_t*)calloc(1, sizeof(struct nio_conf_t));
    if (g_conf == NULL) {
        fprintf(stderr, "no memory.\n");
        return -1;
    }

    /* デフォルト値を設定します。*/
    g_conf->port_no = DEFAULT_PORT;
    g_conf->backlog = DEFAULT_BACKLOG;
    g_conf->worker_threads = DEFAULT_WORKER_THREADS;
    g_conf->nio_bucket_num = DEFAULT_BUCKET_NUM;
    g_conf->nio_mmap_size = MMAP_AUTO_SIZE;

    /* コンフィグファイル名がパラメータで指定されていない場合は
       デフォルトのファイル名を使用します。*/
    if (conf_file == NULL)
        conf_file = DEFAULT_CONF_FILE;

    /* コンフィグファイルの解析 */
    config(conf_file);
    return 0;
}

int main(int argc, char* argv[])
{
    int ret;

    /* パラメータ解析 */
    if (ret = parse(argc, argv)) {
        if (ret < 0)
            usage();
        return 1;
    }

    /* コンフィグファイルの処理 */
    if (parse_config() < 0)
        return 1;

#ifndef WIN32
    if (action == ACT_START) {
        /* ユーザーの切換 */
        if (getuid() == 0 || geteuid() == 0) {
            struct passwd* pw;

            if (g_conf->username[0] == '\0') {
                fprintf(stderr, "can't run as root, please user switch -u\n");
                return 1;
            }
            if ((pw = getpwnam(g_conf->username)) == 0) {
                fprintf(stderr, "can't find the user %s\n", g_conf->username);
                return 1;
            }
            if (setgid(pw->pw_gid) < 0 || setuid(pw->pw_uid) < 0) {
                fprintf(stderr, "change user failed, %s\n", g_conf->username);
                return 1;
            }
        }
    }
#endif

#ifndef _WIN32
    if (action == ACT_START) {
        if (g_conf->daemonize) {
#ifdef MAC_OSX
            if (daemon(1, 0) != 0)
                fprintf(stderr, "daemon() error\n");
#else
            if (daemon(0, 0) != 0)
                fprintf(stderr, "daemon() error\n");
#endif
        }
    }
#endif

    /* 初期処理 */
    if (startup() < 0)
        return 1;

    if (action == ACT_START)
        nio_server();
    else if (action == ACT_STOP)
        stop_server();
    else if (action == ACT_STATUS)
        status_server();

    /* 後処理 */
    cleanup();
    free(g_conf);

#ifdef WIN32
    _CrtDumpMemoryLeaks();
#endif
    return 0;
}
