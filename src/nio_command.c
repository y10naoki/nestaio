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

static void server_cmd(const char* cmd_str)
{
    SOCKET socket;
    char recvbuf[256];
    int okay_flag = 0;

    socket = sock_connect_server("127.0.0.1", g_conf->port_no);
    if (socket != INVALID_SOCKET) {
        char cmdline[64];

        snprintf(cmdline, sizeof(cmdline), "%s\r\n", cmd_str);
        if (send_data(socket, cmdline, strlen(cmdline)) > 0) {
            if (recv_line(socket, recvbuf, sizeof(recvbuf), "\r\n") > 0)
                okay_flag = 1;
        }
    }

    if (okay_flag)
        fprintf(stdout, "\n%s\n\n", recvbuf);
    else
        fprintf(stdout, "\n%s\n\n", "not running.");

    if (socket != INVALID_SOCKET)
        SOCKET_CLOSE(socket);
}

void stop_server()
{
    server_cmd(SHUTDOWN_CMD);
}

void status_server()
{
    server_cmd(STATUS_CMD);
}
