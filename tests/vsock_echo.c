/* SPDX-License-Identifier: ISC */
/* Copyright (c) 2026 Gil Bahat */
/*
 * vsock_echo -- a plain-C AF_VSOCK echo server + client.
 *
 *   vsock_echo server [port]                 accept connections, echo bytes
 *   vsock_echo client [port] [message]       connect, send, print the echo
 *
 * This is written as ordinary Linux-style vsock code -- AF_VSOCK sockets and
 * struct sockaddr_vm, exactly like typical vsock software (an enclave broker,
 * a VM agent) would use. It is the representative consumer used to verify the
 * interposer: on macOS it compiles thanks to the shim in vsock_unix.h and
 * *runs unchanged* under DYLD_INSERT_LIBRARIES=libvsock_unix.dylib, which
 * redirects the AF_VSOCK calls onto Unix sockets via the (cid,port)->path
 * convention.
 *
 * Nothing here references the vsock_unix_* helpers or AF_UNIX -- the whole
 * point is that Linux vsock source needs zero changes.
 */

#include "vsock_unix.h" /* only for the AF_VSOCK / sockaddr_vm shim on macOS */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DEFAULT_PORT 1234

static void die(const char *msg)
{
    perror(msg);
    exit(1);
}

static int run_server(unsigned int port)
{
    struct sockaddr_vm addr;
    struct sockaddr_vm peer;
    socklen_t peerlen = sizeof peer;
    int lfd, cfd;
    char buf[4096];
    ssize_t n;

    lfd = socket(AF_VSOCK, SOCK_STREAM, 0);
    if (lfd < 0)
        die("socket(AF_VSOCK)");

    memset(&addr, 0, sizeof addr);
    addr.svm_family = AF_VSOCK;
    addr.svm_cid = VMADDR_CID_ANY;
    addr.svm_port = port;
    if (bind(lfd, (struct sockaddr *)&addr, sizeof addr) < 0)
        die("bind");
    if (listen(lfd, 1) < 0)
        die("listen");

    fprintf(stderr, "vsock_echo: server listening on port %u\n", port);

    for (;;) {
        cfd = accept(lfd, (struct sockaddr *)&peer, &peerlen);
        if (cfd < 0)
            die("accept");
        fprintf(stderr, "vsock_echo: server accepted (peer cid=%u port=%u)\n",
                peer.svm_cid, peer.svm_port);

        while ((n = read(cfd, buf, sizeof buf)) > 0) {
            ssize_t off = 0;
            while (off < n) {
                ssize_t w = write(cfd, buf + off, (size_t)(n - off));
                if (w < 0)
                    die("write");
                off += w;
            }
        }
        if (n < 0)
            die("read");
        (void)close(cfd);
        fprintf(stderr, "vsock_echo: server connection closed\n");
    }
}

static int run_client(unsigned int port, const char *msg)
{
    struct sockaddr_vm addr;
    int fd;
    size_t msglen = strlen(msg);
    size_t got = 0;
    char buf[4096];

    fd = socket(AF_VSOCK, SOCK_STREAM, 0);
    if (fd < 0)
        die("socket(AF_VSOCK)");

    memset(&addr, 0, sizeof addr);
    addr.svm_family = AF_VSOCK;
    addr.svm_cid = VMADDR_CID_HOST;
    addr.svm_port = port;
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0)
        die("connect");

    fprintf(stderr, "vsock_echo: client connected to port %u\n", port);

    {
        size_t off = 0;
        while (off < msglen) {
            ssize_t w = write(fd, msg + off, msglen - off);
            if (w < 0)
                die("write");
            off += (size_t)w;
        }
    }

    /* Read back exactly msglen bytes of echo. */
    while (got < msglen) {
        ssize_t n = read(fd, buf + got, sizeof buf - got);
        if (n < 0)
            die("read");
        if (n == 0)
            break;
        got += (size_t)n;
    }
    (void)close(fd);

    if (got != msglen || memcmp(buf, msg, msglen) != 0) {
        fprintf(stderr, "vsock_echo: MISMATCH (sent %zu, got %zu)\n", msglen,
                got);
        return 1;
    }
    fprintf(stdout, "%.*s\n", (int)got, buf);
    fprintf(stderr, "vsock_echo: client OK (%zu bytes echoed)\n", got);
    return 0;
}

int main(int argc, char **argv)
{
    unsigned int port = DEFAULT_PORT;

    if (argc < 2) {
        fprintf(stderr, "usage: %s server|client [port] [message]\n", argv[0]);
        return 2;
    }
    if (argc >= 3)
        port = (unsigned int)strtoul(argv[2], NULL, 10);

    if (strcmp(argv[1], "server") == 0)
        return run_server(port);
    if (strcmp(argv[1], "client") == 0)
        return run_client(port, argc >= 4 ? argv[3] : "hello vsock");

    fprintf(stderr, "usage: %s server|client [port] [message]\n", argv[0]);
    return 2;
}
