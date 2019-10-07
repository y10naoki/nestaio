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

/*
 * ハッシュデータベースに memcachedプロトコルを実装したものです。
 *
 * <<memcachedプロトコル仕様書>>
 * http://code.sixapart.com/svn/memcached/trunk/server/doc/protocol.txt
 *
 * データは最大1MBまでに制限されています。
 * キー長は250バイトまでに制限されています。
 *
 * データの先頭にヘッダーが格納されます（現在は 8バイトになります）。
 * ヘッダーサイズがデータの先頭に(8ビット値)として格納されます。
 * <flags>はヘッダーサイズの後に(32ビット値)として格納されます。
 * <exptime>は<flags>の後に(32ビット値)として格納されます。
 *
 * [データ]
 * +----+-------+---------+------------+
 * |size|<flags>|<exptime>|<data block>|
 * +----+-------+---------+------------+
 *
 * データの保存を行うコマンド(set,add,replace,append,prepend)は、
 * 以下のような文法となります。
 *
 *    <コマンド> <key> <flags> <exptime> <bytes>\r\n
 *    <data block>\r\n
 *
 *     <key>は保存するためのキー名を指定します。最大長は250バイトです。
 *     <flags>はアプリケーション特有の32bitの値(0〜4294967295)を
 *     指定することができ、データの取得時に格納した時の値が返されます。
 *     <exptime>はデータの有効期間を秒数で指定します。
 *     <bytes>は以下の<data block>で指定するデータのサイズです。
 *     例えば "abcde" と5文字を格納する場合は、5と指定します。
 *     <data block>は格納するデータです。最大長は約1MBです。
 *
 *    casコマンドは getsコマンドで取得した <cas unique> で
 *    楽観的排他制御(compare and swap)を実現します。
 *
 *    cas <key> <flags> <exptime> <bytes> <cas unique>\r\n
 *    <data block>\r\n
 *
 * データの取得・削除を行うコマンド(get,gets,delete)は、
 * 以下のような文法となり、引数に対象のキー名を指定します。
 *
 *    <コマンド> <key>
 *
 * データは以下の形式で返されます。
 *
 *    VALUE <key> <flags> <bytes>\r\n
 *    <data block>\r\n
 *
 * データを楽観的排他制御で更新する場合の取得コマンド(gets)は、
 * 以下の形式で返されます。
 *
 *    VALUE <key> <flags> <bytes> <cas unique>\r\n
 *    <data block>\r\n
 *
 * 値の加減算を行うコマンド(incr,decr)は、
 * 以下のような文法となり、引数に対象のキー名と加減算する数値を指定します。
 * value は 符号なし64bit整数として処理されます。
 *
 *    <コマンド> <key> <value>
 *
 * 各種ステータスを表示する stats コマンドには対応していません。
 *
 * 終了 quit コマンドで接続が遮断されます。
 *
 * 2010/10/16
 * レプリケーション用のコマンドを追加。
 *
 *  【データ取得コマンド】
 *    bget <key><CRLF>
 *
 *    （応答フォーマット）
 *    +-+---------+---------+--------+------------+
 *    |V|<size>(4)|<stat>(1)|<cas>(8)|<data>(size)|
 *    +-+---------+---------+--------+------------+
 *    |*|<-------------- datablock -------------->|
 *
 *    先頭バイトが "V" でその後に32ビットの<size>が続きます。
 *    <size>は<data>のバイト数になります。
 *    <stat>に DATA_COMPRESS_Z ビットが立っている場合は<data>が
 *    zlib で圧縮されています。
 *    この場合の<size>は圧縮後のバイト数が設定されます。
 *
 *  【データ設定コマンド】
 *    bset <key><CRLF>
 *    <datablock>
 *
 *    （datablockフォーマット）
 *    +---------+---------+--------+------------+
 *    |<size>(4)|<stat(1)>|<cas(8)>|<data>(size)|
 *    +---------+---------+--------+------------+
 *    <size>は<data>のバイト数になります。
 *    <stat>に DATA_COMPRESS_Z ビットが立っている場合は<data>が
 *    zlib で圧縮されています。
 *    この場合の<size>は圧縮後のバイト数が設定されます。
 *
 *    <datablock> の後には <CRLF> は付きません。
 *
 *    （応答フォーマット）
 *    +--+
 *    |OK|
 *    +--+
 *    エラーの場合は "ER" が返されます。
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "nio_server.h"

#define CMD_SET         1   /* データの保存(キーが存在している場合は置換) */
#define CMD_ADD         2   /* データの保存(キーが既に存在しない場合のみ) */
#define CMD_REPLACE     3   /* データの保存(キーが既に存在する場合のみ) */
#define CMD_APPEND      4   /* 値への後方追加 */
#define CMD_PREPEND     5   /* 値への前方追加 */
#define CMD_CAS         6   /* データの保存(バージョン排他制御) */
#define CMD_GET         7   /* データの取得 */
#define CMD_GETS        8   /* データの取得(バージョン付き) */
#define CMD_DELETE      9   /* データの削除 */
#define CMD_FLUSH_ALL   10  /* 全件データの削除 */
#define CMD_INCR        11  /* 値への加算 */
#define CMD_DECR        12  /* 値への減算 */
#define CMD_STATS       13  /* 各種ステータスを表示 */
#define CMD_VERSION     14  /* バージョンを表示 */
#define CMD_VERBOSITY   15  /* 動作確認 */
#define CMD_QUIT        30  /* 終了(コネクション切断) */
#define CMD_STATUS      100 /* ステータス確認 */
#define CMD_SHUTDOWN    110 /* 終了(シャットダウン) */
#define CMD_BGET        200 /* レプリケーション用 get */
#define CMD_BSET        201 /* レプリケーション用 set */
#define CMD_BKEYS       202 /* 再分配用 get all keys */

#define VERSION_STR PROGRAM_VERSION

#define DATABLOCK_HEADER_SIZE   (sizeof(uchar)+sizeof(uint)+sizeof(uint))

#define MAX_MEMCACHED_KEYSIZE   250
#define MAX_MEMCACHED_DATASIZE  (1*1024*1024+DATABLOCK_HEADER_SIZE)   /* 1MB + data block header */

#define CHECK_NONE      0
#define CHECK_ADD       1
#define CHECK_REPLACE   2

#define UPDATE_APPEND   1
#define UPDATE_PREPEND  2

#define MODE_INCR       1
#define MODE_DECR       2

#define STORE_STORED     0
#define STORE_NOT_STORED -1
#define STORE_EXISTS     -2
#define STORE_NOT_FOUND  -3

#define LINE_DELIMITER  "\r\n"

#define DATA_COMPRESS_Z     0x01

#define STAT_FIN       0x01
#define STAT_CLOSE     0x02
#define STAT_SHUTDOWN  0x04

#ifdef WIN32
static HANDLE memcached_queue_cond;
#else
static pthread_mutex_t memcached_queue_mutex;
static pthread_cond_t memcached_queue_cond;
#endif

static int open_database()
{
    /* データベースの初期化 */
    g_conf->nio_db = nio_initialize(NIO_HASH);
    if (g_conf->nio_db == NULL) {
        err_write("memcached: nio_initialize() error.");
        return -1;
    }

    /* プロパティの設定 */
    if (g_conf->nio_bucket_num != 0) {
        if (nio_property(g_conf->nio_db, NIO_BUCKET_NUM, g_conf->nio_bucket_num) < 0)
            err_write("memcached: nio_property() bucket number error value=%d",
                      g_conf->nio_bucket_num);
    }
    if (g_conf->nio_mmap_size != 0) {
        if (nio_property(g_conf->nio_db, NIO_MAP_VIEWSIZE, g_conf->nio_mmap_size) < 0)
            err_write("memcached: nio_property() mmap size error value=%d", g_conf->nio_mmap_size);
    }

    /* データベースのオープン */
    if (nio_file(g_conf->nio_db, g_conf->nio_path)) {
        if (nio_open(g_conf->nio_db, g_conf->nio_path) < 0) {
            nio_finalize(g_conf->nio_db);
            err_write("memcached: nio_open() error file=%s", g_conf->nio_path);
            return -1;
        }
    } else {
        if (nio_create(g_conf->nio_db, g_conf->nio_path) < 0) {
            nio_finalize(g_conf->nio_db);
            err_write("memcached: nio_create() error file=%s", g_conf->nio_path);
            return -1;
        }
    }
    return 0;
}

static void close_database()
{
    if (g_conf->nio_db) {
        nio_close(g_conf->nio_db);
        nio_finalize(g_conf->nio_db);
    }
}

static int parse_command(const char* str)
{
    if (stricmp(str, "set") == 0)
        return CMD_SET;
    if (stricmp(str, "add") == 0)
        return CMD_ADD;
    if (stricmp(str, "replace") == 0)
        return CMD_REPLACE;
    if (stricmp(str, "append") == 0)
        return CMD_APPEND;
    if (stricmp(str, "prepend") == 0)
        return CMD_PREPEND;
    if (stricmp(str, "cas") == 0)
        return CMD_CAS;
    if (stricmp(str, "get") == 0)
        return CMD_GET;
    if (stricmp(str, "gets") == 0)
        return CMD_GETS;
    if (stricmp(str, "delete") == 0)
        return CMD_DELETE;
    if (stricmp(str, "incr") == 0)
        return CMD_INCR;
    if (stricmp(str, "decr") == 0)
        return CMD_DECR;
    if (stricmp(str, "stats") == 0)
        return CMD_STATS;
    if (stricmp(str, "version") == 0)
        return CMD_VERSION;
    if (stricmp(str, "verbosity") == 0)
        return CMD_VERBOSITY;
    if (stricmp(str, "flush_all") == 0)
        return CMD_FLUSH_ALL;
    if (stricmp(str, "quit") == 0)
        return CMD_QUIT;

    if (strcmp(str, SHUTDOWN_CMD) == 0)
        return CMD_SHUTDOWN;
    if (strcmp(str, STATUS_CMD) == 0)
        return CMD_STATUS;

    if (stricmp(str, "bget") == 0)
        return CMD_BGET;
    if (stricmp(str, "bset") == 0)
        return CMD_BSET;
    if (stricmp(str, "bkeys") == 0)
        return CMD_BKEYS;

    return -1;
}

static int cmd_error(SOCKET socket)
{
    char* buf = "ERROR\r\n";

    if (send_data(socket, buf, strlen(buf)) < 0) {
        err_write("memcached: cmd_error() send failed.");
        return -1;
    }
    return 0;
}

static int client_error(SOCKET socket, const char* err_msg)
{
    char buf[1024];

    snprintf(buf, sizeof(buf), "ERROR %s\r\n", err_msg);
    if (send_data(socket, buf, strlen(buf)) < 0) {
        err_write("memcached: client_error() error.");
        return -1;
    }
    return 0;
}

static int server_error(SOCKET socket, const char* err_msg)
{
    char buf[1024];

    snprintf(buf, sizeof(buf), "SERVER_ERROR %s\r\n", err_msg);
    if (send_data(socket, buf, strlen(buf)) < 0) {
        err_write("memcached: server_error() error.");
        return -1;
    }
    return 0;
}

static int noreply(int n, const char** p)
{
    if (n > 1)
        return (stricmp(p[n-1], "noreply") == 0);
    return 0;
}

static int store_args_check(SOCKET socket, int cn, const char** cl, int args)
{
    if (cn < args) {
        if (! noreply(cn, cl)) {
            char msg[256];

            snprintf(msg, sizeof(msg), "illegal command line.");
            client_error(socket, msg);
        }
        return -1;
    }
    return 0;
}

static int store_size_check(SOCKET socket, const char* key, int bytes, int noreply_flag)
{
    char msg[256];

    if (strlen(key) > MAX_MEMCACHED_KEYSIZE) {
        if (! noreply_flag) {
            snprintf(msg, sizeof(msg), "key size too long %d <= %d",
                     (int)strlen(key), MAX_MEMCACHED_KEYSIZE);
            client_error(socket, msg);
        }
        return -1;
    }
    if (bytes < 0) {
        if (! noreply_flag) {
            snprintf(msg, sizeof(msg), "illegal bytes %d", bytes);
            client_error(socket, msg);
        }
        return -1;
    }
    if (bytes > MAX_MEMCACHED_DATASIZE) {
        if (! noreply_flag) {
            snprintf(msg, sizeof(msg), "data too long %d <= 1MB", bytes);
            client_error(socket, msg);
        }
        return -1;
    }
    return 0;
}

static int check_expier(uint exptime, const char* key, int keysize)
{
    if (exptime > 0) {
        if (exptime < system_seconds()) {
            /* 生存期間を過ぎているため削除します。 */
            nio_delete(g_conf->nio_db, key, keysize);
            return 1;
        }
    }
    return 0;
}

static void dust_recv_buffer(struct sock_buf_t* sb)
{
    int end_flag = 0;

    while (! end_flag) {
        char buf[BUF_SIZE];

        if (! sockbuf_wait_data(sb, RCV_TIMEOUT_NOWAIT))
            break;  /* empty */
        if (sockbuf_gets(sb, buf, sizeof(buf), LINE_DELIMITER, 0, &end_flag) < 1)
            break;
    }
}

static int datablock_recv(struct sock_buf_t* sb, int cn, const char** cl, char* buf, int bytes)
{
    int bufsize;
    int len;
    int line_flag;
    int data_err = 0;

    /* bufsize: (bytes + strlen(CRLF) + NULL terminate) */
    bufsize = bytes  + strlen(LINE_DELIMITER) + 1;
    len = sockbuf_gets(sb, buf, bufsize, LINE_DELIMITER, 0, &line_flag);
    if (len < 1)
        return -1;
    if (! line_flag) {
        /* 行末(CRLF)まで読み捨てます。*/
        dust_recv_buffer(sb);
        data_err = 1;
        err_write("datablock_recv() not found <CRLF> socket=%d, len=%d", sb->socket, len);
    }
    if (len != bytes)
        data_err = 1;

    if (data_err) {
        if (! noreply(cn, cl)) {
            char msg[256];

            snprintf(msg, sizeof(msg), "<data block> size error, socket=%d, req bytes=%d, recv len=%d", sb->socket, bytes, len);
            client_error(sb->socket, msg);
        }
        return -1;
    }
    return 0;
}

static int store_response(SOCKET socket, int result)
{
    char* reply_str;

    /* 応答データ */
    if (result == STORE_STORED)
        reply_str = "STORED\r\n";
    else if (result == STORE_EXISTS)
        reply_str = "EXISTS\r\n";
    else if (result == STORE_NOT_FOUND)
        reply_str = "NOT_FOUND\r\n";
    else
        reply_str = "NOT_STORED\r\n";

    if (send_data(socket, reply_str, strlen(reply_str)) < 0) {
        err_write("memcached: store_response() send error.");
        return -1;
    }
    return 0;
}

static void set_data_header(char* buf, uint flags, uint exptime)
{
    uchar size = DATABLOCK_HEADER_SIZE - sizeof(uchar);

    memcpy(buf, &size, sizeof(uchar));
    memcpy(&buf[sizeof(uchar)], &flags, sizeof(uint));
    memcpy(&buf[sizeof(uchar)+sizeof(uint)], &exptime, sizeof(uint));
}

static void get_data_header(const char* buf, uint* flags, uint* exptime)
{
    uchar size;

    memcpy(&size, buf, sizeof(uchar));
    if (flags)
        memcpy(flags, &buf[sizeof(uchar)], sizeof(uint));
    if (exptime)
        memcpy(exptime, &buf[sizeof(uchar)+sizeof(uint)], sizeof(uint));
}

static int set(struct sock_buf_t* sb,
               int cn,
               const char** cl,
               int args,
               int cas_flag,
               int check_mode)
{
    int result = 0;
    char* key;
    char* flags_s;
    char* exptime_s;
    char* bytes_s;
    uint flags;
    uint exptime;
    int bytes;
    int64 cas = 0;
    int bufsize;
    char* buf;

    if (store_args_check(sb->socket, cn, cl, args) < 0)
        return -1;

    key = trim((char*)cl[1]);

    flags_s = trim((char*)cl[2]);
    if (! isdigitstr(flags_s))
        return -1;
    flags = (uint)atoi(flags_s);

    exptime_s = trim((char*)cl[3]);
    if (! isdigitstr(exptime_s))
        return -1;
    exptime = (uint)atoi(exptime_s);
    if (exptime > 0)
        exptime += system_seconds();

    bytes_s = trim((char*)cl[4]);
    if (! isdigitstr(bytes_s))
        return -1;
    bytes = atoi(bytes_s);

    if (cas_flag) {
        char* cas_s;

        cas_s = trim((char*)cl[5]);
        if (! isdigitstr(cas_s))
            return -1;
        cas = atoi64(cas_s);
    }
    if (store_size_check(sb->socket, key, bytes, noreply(cn, cl)) < 0)
        return -1;

    /* data block を socket から取得します。*/
    bufsize = DATABLOCK_HEADER_SIZE + bytes;
    buf = (char*)malloc(bufsize + strlen(LINE_DELIMITER) + 1);
    if (buf == NULL) {
        err_write("memcached: set() no memory.");
        return -1;
    }
    set_data_header(buf, flags, exptime);
    if (datablock_recv(sb, cn, cl, &buf[DATABLOCK_HEADER_SIZE], bytes) < 0) {
        free(buf);
        return -1;
    }
    if (check_mode != CHECK_NONE) {
        char* dbuf;
        int dsize = 0;

        dbuf = nio_aget(g_conf->nio_db, key, strlen(key), &dsize);
        if (dbuf) {
            uint dexptime;

            memcpy(&dexptime, &dbuf[sizeof(uint)], sizeof(uint));
            if (check_expier(dexptime, key, strlen(key)))
                dsize = 0;
            nio_free(g_conf->nio_db, dbuf);
        }

        if (check_mode == CHECK_ADD) {
            if (dsize >= 0) {
                /* すでにキーが存在していたらエラー */
                if (! noreply(cn, cl))
                    store_response(sb->socket, STORE_EXISTS);
                free(buf);
                return -1;
            }
        } else if (check_mode == CHECK_REPLACE) {
            if (dsize < 0) {
                /* キーが存在していないとエラー */
                if (! noreply(cn, cl))
                    store_response(sb->socket, STORE_NOT_FOUND);
                free(buf);
                return -1;
            }
        }
    }

    /* データベースへ出力 */
    if (cas_flag)
        result = nio_puts(g_conf->nio_db, key, strlen(key), buf, bufsize, cas);
    else
        result = nio_put(g_conf->nio_db, key, strlen(key), buf, bufsize);

    if (! noreply(cn, cl))
        store_response(sb->socket, result);

    free(buf);
    return result;
}

static int update(struct sock_buf_t* sb, int cn, const char** cl, int mode)
{
    int result = 0;
    char* key;
    char* bytes_s;
    int bytes;
    int64 cas;
    char* buf;
    int dsize;
    char* dbuf;
    uint dexptime;
    char* tbuf;

    if (store_args_check(sb->socket, cn, cl, 5) < 0)
        return -1;

    key = trim((char*)cl[1]);

    bytes_s = trim((char*)cl[4]);
    if (! isdigitstr(bytes_s))
        return -1;
    bytes = atoi(bytes_s);

    if (store_size_check(sb->socket, key, bytes, noreply(cn, cl)) < 0)
        return -1;

    /* data block を socket から取得します。*/
    buf = (char*)malloc(bytes + strlen(LINE_DELIMITER) + 1);
    if (buf == NULL) {
        err_write("memcached: update() no memory.");
        return -1;
    }
    if (datablock_recv(sb, cn, cl, buf, bytes) < 0) {
        free(buf);
        return -1;
    }

    /* キーの存在チェック */
    dbuf = nio_agets(g_conf->nio_db, key, strlen(key), &dsize, &cas);
    if (dbuf == NULL) {
        if (! noreply(cn, cl))
            store_response(sb->socket, STORE_NOT_FOUND);
        free(buf);
        return -1;
    }

    /* データサイズのチェック */
    if (store_size_check(sb->socket, key, dsize-sizeof(uint)+bytes, noreply(cn, cl)) < 0) {
        nio_free(g_conf->nio_db, dbuf);
        free(buf);
        return -1;
    }

    get_data_header(dbuf, NULL, &dexptime);
    if (check_expier(dexptime, key, strlen(key))) {
        if (! noreply(cn, cl))
            store_response(sb->socket, STORE_NOT_FOUND);
        nio_free(g_conf->nio_db, dbuf);
        free(buf);
        return -1;
    }

    /* 編集用のバッファを確保します。*/
    tbuf = (char*)malloc(dsize + bytes);
    if (tbuf == NULL) {
        err_write("memcached: update() no memory.");
        nio_free(g_conf->nio_db, dbuf);
        free(buf);
        return -1;
    }
    memcpy(tbuf, dbuf, dsize);
    nio_free(g_conf->nio_db, dbuf);

    /* バッファを編集します。*/
    if (mode == UPDATE_APPEND) {
        memcpy(tbuf+dsize, buf, bytes);
    } else if (mode == UPDATE_PREPEND) {
        memmove(&tbuf[DATABLOCK_HEADER_SIZE+bytes],
                &tbuf[DATABLOCK_HEADER_SIZE],
                dsize-DATABLOCK_HEADER_SIZE);
        memcpy(&tbuf[DATABLOCK_HEADER_SIZE], buf, bytes);
    } else {
        if (! noreply(cn, cl))
            server_error(sb->socket, "internal update mode error.");
        free(tbuf);
        free(buf);
        return -1;
    }

    /* データベースへ出力 */
    result = nio_puts(g_conf->nio_db, key, strlen(key), tbuf, dsize + bytes, cas);

    if (! noreply(cn, cl))
        store_response(sb->socket, result);

    free(tbuf);
    free(buf);
    return result;
}

/* set <key> <flags> <exptime> <bytes> [noreply]
 * <data block>
 */
static int set_command(struct sock_buf_t* sb, int cn, const char** cl)
{
    return set(sb, cn, cl, 5, 0, CHECK_NONE);
}

/* add <key> <flags> <exptime> <bytes> [noreply]
 * <data block>
 */
static int add_command(struct sock_buf_t* sb, int cn, const char** cl)
{
    return set(sb, cn, cl, 5, 0, CHECK_ADD);
}

/* replace <key> <flags> <exptime> <bytes> [noreply]
 * <data block>
 */
static int replace_command(struct sock_buf_t* sb, int cn, const char** cl)
{
    return set(sb, cn, cl, 5, 0, CHECK_REPLACE);
}

/* append <key> <flags> <exptime> <bytes> [noreply]
 * <data block>
 *
 * ignore <flags> and <exptime>
 */
static int append_command(struct sock_buf_t* sb, int cn, const char** cl)
{
    return update(sb, cn, cl, UPDATE_APPEND);
}

/* prepend <key> <flags> <exptime> <bytes> [noreply]
 * <data block>
 *
 * ignore <flags> and <exptime>
 */
static int prepend_command(struct sock_buf_t* sb, int cn, const char** cl)
{
    return update(sb, cn, cl, UPDATE_PREPEND);
}

/* cas <key> <flags> <exptime> <bytes> <cas unqiue> [noreply]
 * <data block>
 */
static int cas_command(struct sock_buf_t* sb, int cn, const char** cl)
{
    return set(sb, cn, cl, 6, 1, CHECK_NONE);
}

static int get_element(const char* key, int cas_flag, struct membuf_t* mb)
{
    int dsize;
    char* dbuf;
    int64 cas;
    uint flags;
    uint exptime;
    int bytes;
    char value_buf[128+MAX_MEMCACHED_KEYSIZE];

    dbuf = nio_agets(g_conf->nio_db, key, strlen(key), &dsize, &cas);
    if (dbuf == NULL)
        return 0;

    if (dsize > MAX_MEMCACHED_DATASIZE) {
        nio_free(g_conf->nio_db, dbuf);
        return 0;
    }

    get_data_header(dbuf, &flags, &exptime);
    if (check_expier(exptime, key, strlen(key))) {
        nio_free(g_conf->nio_db, dbuf);
        return 0;
    }
    bytes = dsize - DATABLOCK_HEADER_SIZE;

    /* 応答データの編集 */
    if (cas_flag)
        snprintf(value_buf, sizeof(value_buf), "VALUE %s %u %d %lld\r\n", key, flags, bytes, cas);
    else
        snprintf(value_buf, sizeof(value_buf), "VALUE %s %u %d\r\n", key, flags, bytes);

    mb_append(mb, value_buf, strlen(value_buf));
    mb_append(mb, &dbuf[DATABLOCK_HEADER_SIZE], bytes);
    mb_append(mb, "\r\n", sizeof("\r\n")-1);

    nio_free(g_conf->nio_db, dbuf);
    return 0;
}

static int get(struct sock_buf_t* sb, int cn, const char** cl, int cas_flag)
{
    char** keys;
    struct membuf_t* mb;
    char* end_str = "END\r\n";

    if (cn < 2)
        return client_error(sb->socket, "illegal command line.");

    mb = mb_alloc(1024);
    if (mb == NULL) {
        err_write("memcached: get() no memory.");
        return server_error(sb->socket, "no memory.");
    }

    keys = (char**)&cl[1];
    while (*keys) {
        if (get_element(trim(*keys), cas_flag, mb) < 0) {
            mb_free(mb);
            return server_error(sb->socket, "no memory.");
        }
        keys++;
    }

    /* "END\r\n" の追加 */
    mb_append(mb, end_str, strlen(end_str));

    /* データの送信 */
    if (send_data(sb->socket, mb->buf, mb->size) < 0) {
        err_write("memcached: get_command() response error.");
        mb_free(mb);
        return -1;
    }

    mb_free(mb);
    return 0;
}

/* get <key[ key1 key2 ...]>
 * VALUE <key> <flags> <bytes>
 * <data block>
 * ...
 * END
 */
static int get_command(struct sock_buf_t* sb, int cn, const char** cl)
{
    return get(sb, cn, cl, 0);
}

/* gets <key[ key1 key2 ...]>
 * VALUE <key> <flags> <bytes> <cas unique>
 * <data block>
 * ...
 * END
 */
static int gets_command(struct sock_buf_t* sb, int cn, const char** cl)
{
    return get(sb, cn, cl, 1);
}

/* delete <key> [<time>] [noreply]
 */
static int delete_command(struct sock_buf_t* sb, int cn, const char** cl)
{
    char* key;
    int result;
    char msg[256];

    if (cn < 2) {
        if (! noreply(cn, cl)) {
            snprintf(msg, sizeof(msg), "illegal command line.");
            return client_error(sb->socket, msg);
        }
        return -1;
    }
    key = trim((char*)cl[1]);
    if (strlen(key) > MAX_MEMCACHED_KEYSIZE) {
        if (! noreply(cn, cl)) {
            snprintf(msg, sizeof(msg), "key size too long %d <= %d",
                     (int)strlen(key), MAX_MEMCACHED_KEYSIZE);
            return client_error(sb->socket, msg);
        }
        return -1;
    }

    result = nio_delete(g_conf->nio_db, key, strlen(key));

    if (! noreply(cn, cl)) {
        char* reply_str;

        /* 応答データ */
        reply_str = (result == 0)? "DELETED\r\n" : "NOT_FOUND\r\n";
        if (send_data(sb->socket, reply_str, strlen(reply_str)) < 0) {
            err_write("memcached: delete_command() response error.");
            return -1;
        }
    }
    return 0;
}

/* flush_all
 */
static int flush_all_command(struct sock_buf_t* sb, int cn, const char** cl)
{
    int result = 0;
    char* reply_str;

    /* データベースファイルを一旦クローズして
       新規作成することで全データを削除します。*/
    nio_close(g_conf->nio_db);
    if (nio_create(g_conf->nio_db, g_conf->nio_path) < 0) {
        err_write("memcached: flush_all_command() nio_create() error file=%s", g_conf->nio_path);
        result = -1;
    }

    /* 応答データ */
    reply_str = (result == 0)? "DELETED\r\n" : "ERROR\r\n";
    if (send_data(sb->socket, reply_str, strlen(reply_str)) < 0) {
        err_write("memcached: flush_all_command() response error.");
        return -1;
    }
    return 0;
}

static int incr(struct sock_buf_t* sb, int cn, const char** cl, int mode)
{
    char* key;
    int result = 0;
    char msg[256];
    int dsize;
    char* dbuf;
    uint flags;
    uint exptime;
    uint64 val;
    uint64 incr;
    int64 cas;

    if (cn < 3) {
        if (! noreply(cn, cl)) {
            snprintf(msg, sizeof(msg), "illegal command line.");
            return client_error(sb->socket, msg);
        }
        return -1;
    }
    key = trim((char*)cl[1]);
    if (strlen(key) > MAX_MEMCACHED_KEYSIZE) {
        if (! noreply(cn, cl)) {
            snprintf(msg, sizeof(msg), "key size too long %d <= %d",
                     (int)strlen(key), MAX_MEMCACHED_KEYSIZE);
            return client_error(sb->socket, msg);
        }
        return -1;
    }

    dbuf = nio_agets(g_conf->nio_db, key, strlen(key), &dsize, &cas);
    if (dbuf == NULL) {
        result = -1;
        goto reply;
    }

    if (dsize != (DATABLOCK_HEADER_SIZE + sizeof(uint64))) {
        nio_free(g_conf->nio_db, dbuf);
        if (! noreply(cn, cl)) {
            snprintf(msg, sizeof(msg), "data type error.");
            return client_error(sb->socket, msg);
        }
        return -1;
    }

    get_data_header(dbuf, &flags, &exptime);
    if (check_expier(exptime, key, strlen(key))) {
        result = -1;
        goto reply;
    }
    memcpy(&val, &dbuf[DATABLOCK_HEADER_SIZE], sizeof(uint64));

    incr = (uint64)atoi64(cl[2]);
    if (mode == MODE_INCR)
        val += incr;
    else if (mode == MODE_DECR)
        val -= incr;
    memcpy(&dbuf[DATABLOCK_HEADER_SIZE], &val, sizeof(uint64));

    /* データベースへ書き出します。*/
    result = nio_puts(g_conf->nio_db, key, strlen(key), dbuf, dsize, cas);

reply:
    nio_free(g_conf->nio_db, dbuf);
    if (! noreply(cn, cl)) {
        char valbuf[64];
        char* reply_str;

        /* 応答データ */
        if (result == 0) {
            snprintf(valbuf, sizeof(valbuf), "%llu\r\n", val);
            reply_str = valbuf;
        } else {
            reply_str = "NOT_FOUND\r\n";
        }
        if (send_data(sb->socket, reply_str, strlen(reply_str)) < 0) {
            err_write("memcached: incr_command() response error.");
            return -1;
        }
    }
    return 0;
}

/* incr <key> <value> [noreply]
 */
static int incr_command(struct sock_buf_t* sb, int cn, const char** cl)
{
    return incr(sb, cn, cl, MODE_INCR);
}

/* decr <key> <value> [noreply]
 */
static int decr_command(struct sock_buf_t* sb, int cn, const char** cl)
{
    return incr(sb, cn, cl, MODE_DECR);
}

/* stats
 */
static int stats_command(struct sock_buf_t* sb)
{
    /* 応答データ */
    if (send_data(sb->socket, "\r\n", strlen("\r\n")) < 0) {
        err_write("memcached: stats send error.");
        return -1;
    }
    return 0;
}

/* version
 */
static int version_command(struct sock_buf_t* sb)
{
    char verstr[256];

    snprintf(verstr, sizeof(verstr), "%s\r\n", VERSION_STR);

    /* 応答データ */
    if (send_data(sb->socket, verstr, strlen(verstr)) < 0) {
        err_write("memcached: version send error.");
        return -1;
    }
    return 0;
}

/* verbosity
 */
static int verbosity_command(struct sock_buf_t* sb)
{
    /* 応答データ */
    if (send_data(sb->socket, "OK\r\n", strlen("OK\r\n")) < 0) {
        err_write("memcached: verbosity send error.");
        return -1;
    }
    return 0;
}

/* bget <key>
 */
static int bget_command(struct sock_buf_t* sb, int cn, const char** cl)
{
    char* key;
    char mark = 'V';
    char* dbuf;
    int dsize;
    int64 cas;
    int size;
    unsigned char stat = 0;
    struct membuf_t* mb;
    int result = 0;

    if (cn < 2)
        return -1;

    key = (char*)cl[1];
    dbuf = nio_agets(g_conf->nio_db, key, strlen(key), &dsize, &cas);
    if (dbuf == NULL) {
        if (dsize == -1) {
            /* not found */
            return 1;
        }
        return -1;
    }
    if (dsize > MAX_MEMCACHED_DATASIZE) {
        nio_free(g_conf->nio_db, dbuf);
        return -1;
    }

    size = dsize;
    if (dsize > 255) {
        char* zbuf;

        /* zlib で圧縮します。*/
        zbuf = gz_comp(dbuf, dsize, &size);
        if (zbuf) {
            if (size < dsize) {
                memcpy(dbuf, zbuf, size);
                stat |= DATA_COMPRESS_Z;
            }
            gz_free(zbuf);
        }
    }

    mb = mb_alloc(size+256);
    if (mb == NULL) {
        err_write("memcached: bget() no memory.");
        nio_free(g_conf->nio_db, dbuf);
        return -1;
    }

    mb_append(mb, (const char*)&mark, sizeof(char));
    mb_append(mb, (const char*)&size, sizeof(int));
    mb_append(mb, (const char*)&stat, sizeof(char));
    mb_append(mb, (const char*)&cas, sizeof(int64));
    mb_append(mb, dbuf, size);

    /* データの送信 */
    if (send_data(sb->socket, mb->buf, mb->size) < 0) {
        result = -1;
        err_write("memcached: bget_command() send error.");
    }
    mb_free(mb);
    nio_free(g_conf->nio_db, dbuf);
    return result;
}

/* bset <key>
 */
static int bset_command(struct sock_buf_t* sb, int cn, const char** cl)
{
    char* key;
    int status;
    int size;
    unsigned char stat;
    int64 cas;
    char* buf;
    int result;
    char* resp_str = "OK";

    if (cn < 2)
        return -1;

    key = (char*)cl[1];

    /* サーバーからの受信データを最大3秒待ちます。*/
    if (! sockbuf_wait_data(sb, 3000)) {
        /* サーバーからの応答がない。*/
        err_write("memcached: bset_command() time out recv data key=%s.", key);
        return -1;
    }

    /* <size>を受信します。*/
    size = sockbuf_int(sb, &status);
    if (size < 1 || status != 0) {
        err_write("memcached: bset_command() recv size error key=%s.", key);
        return -1;
    }

    /* <stat>を受信します。*/
    if (sockbuf_nchar(sb, (char*)&stat, sizeof(char)) != sizeof(char)) {
        err_write("memcached: bset_command() recv stat error key=%s.", key);
        return -1;
    }

    /* <cas>を受信します。*/
    cas = sockbuf_int64(sb, &status);
    if (cas < 1 || status != 0) {
        err_write("memcached: bset_command() recv cas error key=%s.", key);
        return -1;
    }

    /* データブロックの編集 */
    buf = (char*)malloc(size);
    if (buf == NULL) {
        err_write("memcached: bset_command() no memory key=%s size=%d.", key, size);
        return -1;
    }

    /* データを受信します。*/
    if (sockbuf_nchar(sb, buf, size) != size) {
        free(buf);
        err_write("memcached: bset_command() recv data error key=%s size=%d.", key, size);
        return -1;
    }

    if (stat & DATA_COMPRESS_Z) {
        /* 圧縮を展開します。*/
        char* zbuf;
        int row_size;

        zbuf = gz_decomp(buf, size, &row_size);
        if (zbuf) {
            char* tp;

            tp = (char*)realloc(buf, row_size);
            if (tp) {
                buf = tp;
                memcpy(buf, zbuf, row_size);
                size = row_size;
            }
            gz_free(zbuf);
        }
    }

    /* データを更新します。
       バージョンを管理する cas も更新されます。*/
    result = nio_bset(g_conf->nio_db, key, strlen(key), buf, size, cas);
    if (result < 0)
        err_write("memcached: bset_command() nio_bset error key=%s.", key);
    free(buf);

    /* 応答データの送信 */
    if (result < 0)
        resp_str = "ER";
    if (send_data(sb->socket, resp_str, strlen(resp_str)) < 0)
        err_write("memcached: bset_command() send error.");
    return result;
}

static int send_key(SOCKET socket, const char* key, int keysize)
{
    unsigned char ksize;

    ksize = (unsigned char)keysize;
    if (send_data(socket, &ksize, sizeof(ksize)) < 0) {
        err_write("memcached: send_key() keysize=%d send error.", keysize);
        return -1;
    }
    if (keysize > 0) {
        if (send_data(socket, key, keysize) < 0) {
            err_write("memcached: send_key() key=%s send error.", key);
            return -1;
        }
    }
    return 0;
}

/* bkeys
 */
static int bkeys_command(struct sock_buf_t* sb, int cn, const char** cl)
{
    int result = 0;
    struct nio_cursor_t* cur;

    if (cn > 1)
        return -1;

    cur = nio_cursor_open(g_conf->nio_db);
    if (cur == NULL) {
        err_write("memcached: bkeys_command() nio_cursor_open error.");
        return -1;
    }

    while (1) {
        int keysize;
        char key[MAX_MEMCACHED_KEYSIZE+1];

        keysize = nio_cursor_key(cur, key, sizeof(key));
        if (keysize < 1) {
            err_write("memcached: bkeys_command() nio_cursor_key error.");
            result = -1;
            break;
        }
        key[keysize] = '\0';

        /* キーを送信します。*/
        result = send_key(sb->socket, key, keysize);
        if (result < 0)
            break;
        if (nio_cursor_next(cur) != 0) {
            /* 終了 */
            result = send_key(sb->socket, NULL, 0);
            break;
        }
    }
    nio_cursor_close(cur);
    return result;
}

static int cmdline_recv(struct sock_buf_t* sb, char* buf, int size, int* line_flag)
{
    int len;

    len = sockbuf_gets(sb, buf, size, LINE_DELIMITER, 0, line_flag);
    if (len < 1)
        return len;
    if (! *line_flag) {
        /* 行末(CRLF)までを読み捨てます。*/
        dust_recv_buffer(sb);
        return 0;
    }
    return len;
}

static unsigned do_command(struct sock_buf_t* sb, struct in_addr addr)
{
    unsigned stat = 0;
    int result = 0;
    int len;
    char buf[BUF_SIZE];
    int line_flag;
    char** clp;
    int cc;
    int cmd;

    /* コマンド行を受信します。*/
    len = cmdline_recv(sb, buf, sizeof(buf), &line_flag);
    if (len < 0)
        return STAT_FIN|STAT_CLOSE;    /* FIN受信 */
    if (len == 0) {
        if (line_flag)
            return 0;    /* 空文字受信 */
        return STAT_FIN|STAT_CLOSE;    /* FIN受信 */
    }
    if (! line_flag) {
        cmd_error(sb->socket);
        return 0;
    }
    TRACE("request command: %s ...", buf);

    clp = split(buf, ' ');
    if (clp == NULL) {
        cmd_error(sb->socket);
        return 0;
    }
    cc = list_count((const char**)clp);
    if (cc <= 0) {
        list_free(clp);
        cmd_error(sb->socket);
        return 0;
    }

    cmd = parse_command(trim(clp[0]));
    switch (cmd) {
        case CMD_SET:
            result = set_command(sb, cc, (const char**)clp);
            break;
        case CMD_ADD:
            result = add_command(sb, cc, (const char**)clp);
            break;
        case CMD_REPLACE:
            result = replace_command(sb, cc, (const char**)clp);
            break;
        case CMD_APPEND:
            result = append_command(sb, cc, (const char**)clp);
            break;
        case CMD_PREPEND:
            result = prepend_command(sb, cc, (const char**)clp);
            break;
        case CMD_CAS:
            result = cas_command(sb, cc, (const char**)clp);
            break;
        case CMD_GET:
            result = get_command(sb, cc, (const char**)clp);
            break;
        case CMD_GETS:
            result = gets_command(sb, cc, (const char**)clp);
            break;
        case CMD_DELETE:
            result = delete_command(sb, cc, (const char**)clp);
            break;
        case CMD_FLUSH_ALL:
            result = flush_all_command(sb, cc, (const char**)clp);
            break;
        case CMD_INCR:
            result = incr_command(sb, cc, (const char**)clp);
            break;
        case CMD_DECR:
            result = decr_command(sb, cc, (const char**)clp);
            break;
        case CMD_STATS:
            result = stats_command(sb);
            break;
        case CMD_VERSION:
            result = version_command(sb);
            break;
        case CMD_VERBOSITY:
            result = verbosity_command(sb);
            break;
        case CMD_QUIT:
            stat = STAT_CLOSE;
            break;
        case CMD_SHUTDOWN:
        case CMD_STATUS: {
            char ip_addr[256];

            mt_inet_ntoa(addr, ip_addr);
            if (strcmp(ip_addr, "127.0.0.1") == 0) {
                char sendbuf[256];

                if (cmd == CMD_SHUTDOWN) {
                    strcpy(sendbuf, "stopped.\r\n");
                    stat |= STAT_SHUTDOWN;
                } else {
                    strcpy(sendbuf, "running.\r\n");
                }
                if (send_data(sb->socket, sendbuf, strlen(sendbuf)) < 0)
                    result = -1;
            } else {
                if (cmd_error(sb->socket) < 0)
                    result = -1;
            }
            stat |= STAT_CLOSE;
            break;
        }
        case CMD_BGET:
            result = bget_command(sb, cc, (const char**)clp);
            if (result != 0) {
                char emark;

                /* 'n' was not found, 'e' is error */
                emark = (result == 1)? 'n' : 'e';
                if (send_data(sb->socket, &emark, sizeof(emark)) < 0)
                    err_write("memcached: bget_command() send error.");
            }
            break;
        case CMD_BSET:
            result = bset_command(sb, cc, (const char**)clp);
            break;
        case CMD_BKEYS:
            result = bkeys_command(sb, cc, (const char**)clp);
            if (result != 0)
                send_key(sb->socket, NULL, 0);
            break;
        default:
            /* エラー応答データ */
            if (cmd_error(sb->socket) < 0)
                result = -1;
            break;
    }
    list_free(clp);
    TRACE(" result=%d done.\n", result);
    return stat;
}

static void break_signal()
{
    SOCKET c_socket;
    const char dummy = 0x30;

    c_socket = sock_connect_server("127.0.0.1", g_conf->port_no);
    if (c_socket == INVALID_SOCKET) {
        err_write("break_signal: can't open socket: %s", strerror(errno));
        return;
    }
    send_data(c_socket, &dummy, sizeof(dummy));
    SOCKET_CLOSE(c_socket);
}

static struct sock_buf_t* socket_buffer(SOCKET socket)
{
    struct sock_buf_t* sb;
    char sockkey[16];

    snprintf(sockkey, sizeof(sockkey), "%d", socket);
    sb = (struct sock_buf_t*)hash_get(g_sockbuf_hash, sockkey);
    if (sb == NULL) {
        err_write("socket_buffer: not found hash key=%d", socket);
        return NULL;
    }
    if (sb->socket != socket) {
        err_write("socket_buffer: illegal socket %d -> %d", socket, sb->socket);
        return NULL;
    }
    return sb;
}

static void socket_cleanup(struct sock_buf_t* sb)
{
    char sockkey[16];

    sock_event_delete(g_sock_event, sb->socket);

    shutdown(sb->socket, 2);  /* 2: RDWR stop */
    SOCKET_CLOSE(sb->socket);

    snprintf(sockkey, sizeof(sockkey), "%d", sb->socket);
    if (hash_delete(g_sockbuf_hash, sockkey) < 0)
        err_write("socket_cleanup: hash_delete fail, key=%s", sockkey);
    sockbuf_free(sb);
}

static void memcached_thread(void* argv)
{
    /* argv unuse */
    struct thread_args_t* th_args;
    SOCKET socket;
    struct in_addr addr;
    struct sock_buf_t* sb;
    int stat;
    int end_flag;

    while (! g_shutdown_flag) {
#ifndef WIN32
        pthread_mutex_lock(&memcached_queue_mutex);
#endif
        /* キューにデータが入るまで待機します。*/
        while (que_empty(g_queue)) {
#ifdef WIN32
            WaitForSingleObject(memcached_queue_cond, INFINITE);
#else
            pthread_cond_wait(&memcached_queue_cond, &memcached_queue_mutex);
#endif
        }
#ifndef WIN32
        pthread_mutex_unlock(&memcached_queue_mutex);
#endif

        /* キューからデータを取り出します。*/
        th_args = (struct thread_args_t*)que_pop(g_queue);
        if (th_args == NULL)
            continue;

        socket = th_args->socket;
        addr = th_args->sockaddr.sin_addr;

        sb = socket_buffer(socket);
        if (sb == NULL)
            continue;

        do {
            end_flag = 0;
            /* コマンドを受信して処理します。*/
            /* 'quit'コマンドが入力されると STAT_CLOSE が真になります。*/
            /* 'shutdown'コマンドが入力されると STAT_SHUTDOWN と
                STAT_CLOSE が真になります。*/
            stat = do_command(sb, addr);

            if (stat & STAT_CLOSE) {
                /* ソケットをクローズします。*/
                if (g_trace_mode) {
                    char ip_addr[256];

                    mt_inet_ntoa(addr, ip_addr);
                    TRACE("disconnect to %s, socket=%d, done.\n", ip_addr, sb->socket);
                }
                /* ソケットをクローズします。*/
                socket_cleanup(sb);
                end_flag = 1;
            }

            if (! end_flag) {
                if (sb->cur_size < 1)
                    end_flag = 1;
            }
        } while (! end_flag);

        if (! (stat & STAT_CLOSE)) {
            /* コマンド処理が終了したのでイベント通知を有効にします。*/
            sock_event_enable(g_sock_event, sb->socket);
        }
        /* パラメータ領域の解放 */
        free(th_args);

        if (stat & STAT_SHUTDOWN) {
            g_shutdown_flag = 1;
            break_signal();
        }
    }

    /* スレッドを終了します。*/
#ifdef _WIN32
    _endthread();
#endif
}

int memcached_request(SOCKET socket, struct sockaddr_in sockaddr)
{
    struct thread_args_t* th_args;

    /* スレッドへ渡す情報を作成します */
    th_args = (struct thread_args_t*)malloc(sizeof(struct thread_args_t));
    if (th_args == NULL) {
        err_log(sockaddr.sin_addr, "no memory.");
        SOCKET_CLOSE(socket);
        return 0;
    }
    th_args->socket = socket;
    th_args->sockaddr = sockaddr;

    /* リクエストされた情報をキューイング(push)します。*/
    que_push(g_queue, th_args);

    /* キューイングされたことをスレッドへ通知します。*/
#ifdef WIN32
    SetEvent(memcached_queue_cond);
#else
    pthread_mutex_lock(&memcached_queue_mutex);
    pthread_cond_signal(&memcached_queue_cond);
    pthread_mutex_unlock(&memcached_queue_mutex);
#endif
    return 0;
}

int memcached_worker_open()
{
    int i;

    for (i = 0; i < g_conf->worker_threads; i++) {
#ifdef _WIN32
        uintptr_t thread_id;
#else
        pthread_t thread_id;
#endif
        /* スレッドを作成します。
           生成されたスレッドはリクエストキューが空のため、
           待機状態に入ります。*/
#ifdef _WIN32
        thread_id = _beginthread(memcached_thread, 0, NULL);
#else
        pthread_create(&thread_id, NULL, (void*)memcached_thread, NULL);
        /* スレッドの使用していた領域を終了時に自動的に解放します。*/
        pthread_detach(thread_id);
#endif
    }
    return 0;
}

int memcached_open()
{
    struct sockaddr_in sockaddr;
    char ip_addr[256];

    /* データベースをオープンします。*/
    if (open_database() < 0)
        return -1;

    /* セッション・リレー リスニングソケットの作成 */
    g_listen_socket = sock_listen(INADDR_ANY,
                                  g_conf->port_no,
                                  g_conf->backlog,
                                  &sockaddr);
    if (g_listen_socket == INVALID_SOCKET) {
        close_database();
        return -1;  /* error */
    }

    /* 自分自身の IPアドレスを取得します。*/
    sock_local_addr(ip_addr);

    /* スターティングメッセージの表示 */
    TRACE("%s port: %d on %s listening ... %d threads\n",
        PROGRAM_NAME, g_conf->port_no, ip_addr, g_conf->worker_threads);

    /* キューイング制御の初期化 */
#ifdef WIN32
    memcached_queue_cond = CreateEvent(NULL, FALSE, FALSE, NULL);
#else
    pthread_mutex_init(&memcached_queue_mutex, NULL);
    pthread_cond_init(&memcached_queue_cond, NULL);
#endif
    return 0;
}

void memcached_close()
{
    close_database();

#ifdef WIN32
    CloseHandle(memcached_queue_cond);
#else
    pthread_cond_destroy(&memcached_queue_cond);
    pthread_mutex_destroy(&memcached_queue_mutex);
#endif
}
