/* SPDX-License-Identifier: ISC */
/* Copyright (c) 2026 Gil Bahat */
/*
 * vsock-emu -- a tiny host peer for the vsock-over-Unix convention.
 *
 *   vsock-emu listen  <cid> <port>   bind (cid,port), accept one client
 *   vsock-emu connect <cid> <port>   dial (cid,port)
 *
 * Once a connection is established it pumps stdin -> socket and
 * socket -> stdout with poll(2), so a human or a test harness can be the
 * other end of any convention peer (an interposed program, vsock_echo, a
 * freestanding raw-syscall guest).
 *
 * This deliberately talks the convention directly via the vsock_unix_*
 * helpers (AF_UNIX under the hood) rather than AF_VSOCK, so it needs no
 * interposer and works as a plain macOS binary.
 */

#include "vsock_unix.h"

#include <poll.h>
#include <signal.h>

static void usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s listen|connect <cid> <port>\n"
            "  listen   bind (cid,port) and accept one connection\n"
            "  connect  dial (cid,port)\n"
            "then pumps stdin<->socket. Path convention:\n"
            "  ${VSOCK_UNIX_DIR:-${TMPDIR:-/tmp}/vsock-unix}/<cid>.<port>\n",
            argv0);
}

/* Parse an unsigned 32-bit decimal; exit on garbage. */
static unsigned int parse_u32(const char *s, const char *what)
{
    char *end;
    unsigned long v;

    errno = 0;
    v = strtoul(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || v > 0xFFFFFFFFUL) {
        fprintf(stderr, "vsock-emu: invalid %s: '%s'\n", what, s);
        exit(2);
    }
    return (unsigned int)v;
}

/* Pump data both ways until either side closes. Returns 0 on clean EOF. */
static int pump(int sock)
{
    char path_buf[4096];
    struct pollfd pfds[2];
    int stdin_open = 1;

    for (;;) {
        pfds[0].fd = sock;
        pfds[0].events = POLLIN;
        pfds[0].revents = 0;
        pfds[1].fd = stdin_open ? STDIN_FILENO : -1;
        pfds[1].events = POLLIN;
        pfds[1].revents = 0;

        if (poll(pfds, 2, -1) < 0) {
            if (errno == EINTR)
                continue;
            perror("poll");
            return 1;
        }

        /* socket -> stdout */
        if (pfds[0].revents & (POLLIN | POLLHUP | POLLERR)) {
            ssize_t n = read(sock, path_buf, sizeof path_buf);
            if (n < 0) {
                if (errno == EINTR || errno == EAGAIN)
                    continue;
                perror("read(socket)");
                return 1;
            }
            if (n == 0)
                return 0; /* peer closed */
            if (write(STDOUT_FILENO, path_buf, (size_t)n) != n) {
                perror("write(stdout)");
                return 1;
            }
        }

        /* stdin -> socket */
        if (stdin_open && (pfds[1].revents & (POLLIN | POLLHUP | POLLERR))) {
            ssize_t n = read(STDIN_FILENO, path_buf, sizeof path_buf);
            if (n < 0) {
                if (errno == EINTR || errno == EAGAIN)
                    continue;
                perror("read(stdin)");
                return 1;
            }
            if (n == 0) {
                /* EOF on stdin: half-close so the peer sees EOF, keep
                 * draining the socket direction. */
                stdin_open = 0;
                (void)shutdown(sock, SHUT_WR);
                continue;
            }
            {
                ssize_t off = 0;
                while (off < n) {
                    ssize_t w = write(sock, path_buf + off, (size_t)(n - off));
                    if (w < 0) {
                        if (errno == EINTR)
                            continue;
                        perror("write(socket)");
                        return 1;
                    }
                    off += w;
                }
            }
        }
    }
}

int main(int argc, char **argv)
{
    unsigned int cid, port;
    int fd, conn;

    /* Don't die on a peer that closes mid-write. */
    signal(SIGPIPE, SIG_IGN);

    if (argc != 4) {
        usage(argv[0]);
        return 2;
    }
    cid = parse_u32(argv[2], "cid");
    port = parse_u32(argv[3], "port");

    fd = vsock_unix_socket();
    if (fd < 0) {
        perror("socket");
        return 1;
    }

    if (strcmp(argv[1], "listen") == 0) {
        struct sockaddr_un peer;
        socklen_t plen = sizeof peer;
        int one = 1;

        (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        if (vsock_unix_bind_listen(fd, cid, port, 1) < 0) {
            perror("bind/listen");
            return 1;
        }
        fprintf(stderr, "vsock-emu: listening on (cid=%u, port=%u)\n", cid,
                port);
        conn = accept(fd, (struct sockaddr *)&peer, &plen);
        if (conn < 0) {
            perror("accept");
            return 1;
        }
        fprintf(stderr, "vsock-emu: accepted a connection\n");
        (void)close(fd);
    } else if (strcmp(argv[1], "connect") == 0) {
        if (vsock_unix_connect(fd, cid, port) < 0) {
            perror("connect");
            return 1;
        }
        fprintf(stderr, "vsock-emu: connected to (cid=%u, port=%u)\n", cid,
                port);
        conn = fd;
    } else {
        usage(argv[0]);
        return 2;
    }

    return pump(conn);
}
