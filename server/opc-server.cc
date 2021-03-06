// -*- mode: cc; c-basic-offset: 4; indent-tabs-mode: nil; -*-
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation version 2.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://gnu.org/licenses/gpl-2.0.txt>

// Receives http://openpixelcontrol.org/ and updates display.

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>

#include "servers.h"
#include "flaschen-taschen.h"

#define SHOW_REFRESH_RATE 0

struct Header {
    uint8_t y_pos;
    uint8_t command;
    uint8_t size_hi;
    uint8_t size_lo;
};

static bool reliable_read(int fd, void *buf, size_t count) {
    ssize_t r;
    while ((r = read(fd, buf, count)) > 0) {
        count -= r;
        buf = (char*)buf + r;
    }
    return count == 0;
}

// Open server. Return file-descriptor or -1 if listen fails.
// Bind to "bind_addr" (can be NULL, then it is 0.0.0.0) and "port".
static int open_server(const char *bind_addr, int port) {
    if (port > 65535) {
        fprintf(stderr, "OPC: Invalid port %d\n", port);
        return -1;
    }
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        fprintf(stderr, "OPC: creating socket: %s", strerror(errno));
        return -1;
    }

    struct sockaddr_in serv_addr = {0};
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = INADDR_ANY;
    if (bind_addr && !inet_pton(AF_INET, bind_addr, &serv_addr.sin_addr.s_addr)) {
        fprintf(stderr, "OPC: Invalid bind IP address %s\n", bind_addr);
        return -1;
    }
    serv_addr.sin_port = htons(port);
    int on = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    if (bind(s, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) < 0) {
        fprintf(stderr, "OPC: Trouble binding to %s:%d: %s",
                bind_addr ? bind_addr : "0.0.0.0", port,
                strerror(errno));
        return -1;
    }
    return s;
}

static void handle_connection(int fd, FlaschenTaschen *display,
                              ft::Mutex *mutex) {
    bool any_error = false;
    while (!any_error) {
#if SHOW_REFRESH_RATE
        struct timeval start, end;
        gettimeofday(&start, NULL);
#endif

        struct Header h;
        if (!reliable_read(fd, &h, sizeof(h)))
            break;  // lazily assume that we get enough.
        uint16_t size = (uint16_t) h.size_hi << 8 | h.size_lo;
        uint8_t *buffer = new uint8_t[ size ];
        if (!reliable_read(fd, buffer, size))
            any_error = true;
        int leds = size / 3;
        uint8_t *pixel_pos = buffer;
        mutex->Lock();
        for (int x = 0; x < leds; ++x) {
            Color c;
            c.r = *pixel_pos++;
            c.g = *pixel_pos++;
            c.b = *pixel_pos++;

            // Ideally, we'd think that we can have the channel denote the
            // y-axis, then x-axis is the pixel.
            //display->SetPixel(x, h.y_pos, c);

            // .. However OPC seems to be in the mindset that a display
            // is essentially a single, gigantically back and forth
            // wrapped strip. At least in the default 'wall' configuration.
            // Lets accomodate that.
            const int col = x % display->width();
            const int row = x / display->width();
            display->SetPixel(row % 2 == 0 ? col : display->width() - col - 1,
                              row, c);
        }
        display->Send();
        mutex->Unlock();
        delete [] buffer;
#if SHOW_REFRESH_RATE
        gettimeofday(&end, NULL);
        int64_t usec = ((uint64_t)end.tv_sec * 1000000 + end.tv_usec)
            - ((int64_t)start.tv_sec * 1000000 + start.tv_usec);
        printf("\b\b\b\b\b\b\b\b%6.1fHz", 1e6 / usec);
#endif
    }
}

static void run_server(int listen_socket,
                       FlaschenTaschen *display, ft::Mutex *mutex) {
    if (listen(listen_socket, 2) < 0) {
        fprintf(stderr, "OPC: listen() failed: %s", strerror(errno));
        return;
    }
    
    for (;;) {
        struct sockaddr_in client;
        socklen_t socklen = sizeof(client);
        int fd = accept(listen_socket, (struct sockaddr*) &client, &socklen);
        if (fd < 0) return;
        handle_connection(fd, display, mutex);
    }

    close(listen_socket);
}

// public interface
static int server_socket = -1;
bool opc_server_init(int port) {
    server_socket = open_server(NULL, port);
    if (server_socket < 0) {
        perror("Listening on opc socket");
        return false;
    }
    fprintf(stderr, "OPC server ready to listen on %d\n", port);
    return true;
}

void opc_server_run_blocking(FlaschenTaschen *display, ft::Mutex *mutex) {
    run_server(server_socket, display, mutex);
}
