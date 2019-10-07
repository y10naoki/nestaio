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

#include "nio_server.h"

#define R_BUF_SIZE 1024
#define CMD_INCLUDE "include"

/*
 * コンフィグファイルを読んでパラメータを設定します。
 * 既知のパラメータ以外はユーザーパラメータとして登録します。
 * パラメータの形式は "name=value"の形式とします。
 *
 * conf_fname: コンフィグファイル名
 *
 * 戻り値
 *  0: 成功
 * -1: 失敗
 *
 * (config parameters)
 * nio.daemon = 1 or 0 (default is 0, unix only)
 * nio.username = string (default is none)
 * nio.port_no = number (default is 11211)
 * nio.backlog = number (default is 100)
 * nio.worker_threads = number (default is 4)
 * nio.error_file = path/file (default is stderr)
 * nio.output_file = path/file (default is stdout)
 * nio.trace_flag = 1 or 0 (default is 0)
 * nio.database_file = path/file (default is none)
 *
 * include = FILE_NAME
 * ...
 */
int config(const char* conf_fname)
{
    FILE *fp;
    char fpath[MAX_PATH+1];
    char buf[R_BUF_SIZE];
    int err = 0;

    get_abspath(fpath, conf_fname, MAX_PATH);
    if ((fp = fopen(fpath, "r")) == NULL) {
        fprintf(stderr, "file open error: %s\n", conf_fname);
        return -1;
    }

    while (fgets(buf, sizeof(buf), fp) != NULL) {
        int index;
        char name[R_BUF_SIZE];
        char value[R_BUF_SIZE];

        /* コメントの排除 */
        index = indexof(buf, '#');
        if (index >= 0) {
            buf[index] = '\0';
            if (strlen(buf) == 0)
                continue;
        }
        /* 名前と値の分離 */
        index = indexof(buf, '=');
        if (index <= 0)
            continue;

        substr(name, buf, 0, index);
        substr(value, buf, index+1, -1);

        /* 両端のホワイトスペースを取り除きます。*/
        trim(name);
        trim(value);

        if (strlen(name) > MAX_VNAME_SIZE-1) {
            fprintf(stderr, "parameter name too large: %s\n", buf);
            err = -1;
            break;
        }
        if (strlen(value) > MAX_VVALUE_SIZE-1) {
            fprintf(stderr, "parameter value too large: %s\n", buf);
            err = -1;
            break;
        }

        if (stricmp(name, "nio.port_no") == 0) {
            g_conf->port_no = (ushort)atoi(value);
        } else if (stricmp(name, "nio.backlog") == 0) {
            g_conf->backlog = atoi(value);
        } else if (stricmp(name, "nio.worker_threads") == 0) {
            g_conf->worker_threads = atoi(value);
        } else if (stricmp(name, "nio.daemon") == 0) {
            g_conf->daemonize = atoi(value);
        } else if (stricmp(name, "nio.username") == 0) {
            strncpy(g_conf->username, value, sizeof(g_conf->username)-1);
        } else if (stricmp(name, "nio.error_file") == 0) {
            if (strlen(value) > 0)
                get_abspath(g_conf->error_file, value, sizeof(g_conf->error_file)-1);
        } else if (stricmp(name, "nio.output_file") == 0) {
            if (strlen(value) > 0)
                get_abspath(g_conf->output_file, value, sizeof(g_conf->output_file)-1);
        } else if (stricmp(name, "nio.trace_flag") == 0) {
            g_trace_mode = atoi(value);
        } else if (stricmp(name, "nio.database_file") == 0) {
            if (strlen(value) > 0)
                get_abspath(g_conf->nio_path, value, sizeof(g_conf->nio_path)-1);
        } else if (stricmp(name, "nio.nio_bucket_num") == 0) {
            g_conf->nio_bucket_num = atoi(value);
        } else if (stricmp(name, "nio.mmap_size") == 0) {
            g_conf->nio_mmap_size = atoi(value);
        } else if (stricmp(name, CMD_INCLUDE) == 0) {
            /* 他のconfigファイルを再帰処理で読み込みます。*/
            if (config(value) < 0)
                break;
        } else {
            /* ignore */
        }
    }

    fclose(fp);
    return err;
}
