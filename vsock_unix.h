/* SPDX-License-Identifier: ISC */
/* Copyright (c) 2026 Gil Bahat */
/*
 * vsock_unix.h -- emulate AF_VSOCK over Unix domain sockets on macOS.
 *
 * Header-only. Two things live here:
 *
 *   1. A macOS-safe shim of Linux's AF_VSOCK / struct sockaddr_vm, so code
 *      written for Linux vsock compiles unchanged on a Mac behind
 *      `#ifdef __APPLE__`. On Linux (where <linux/vm_sockets.h> already
 *      defines these) the shim is skipped.
 *
 *   2. The (cid,port) -> Unix-socket-path convention plus small libc helpers
 *      that implement it: vsock_unix_socket / _bind_listen / _connect /
 *      _path. Every consumer that agrees on this convention interoperates
 *      (a libc program, the interposer, and a freestanding raw-syscall guest
 *      all rendezvous on the same path).
 *
 * The convention:
 *
 *     ${VSOCK_UNIX_DIR:-${TMPDIR:-/tmp}/vsock-unix}/<cid>.<port>
 *
 * where <cid> and <port> are unsigned decimal. CID realism is intentionally
 * dropped -- (cid,port) is just a path key. VMADDR_CID_ANY (0xFFFFFFFF) is
 * canonicalised to VMADDR_CID_HOST (2) for path formation so that a listener
 * bound to CID_ANY and a client dialling the host CID meet on the same path
 * (the common host-service topology).
 *
 * The whole thing fits under macOS's 104-byte sun_path limit:
 * "/tmp/vsock-unix/" + two uint32s in decimal is ~37 chars.
 */

#ifndef VSOCK_UNIX_H
#define VSOCK_UNIX_H

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 1024
#endif

/* ------------------------------------------------------------------ *
 * 1. AF_VSOCK / sockaddr_vm shim (only where the platform lacks it).
 * ------------------------------------------------------------------ */

/*
 * Recent macOS SDKs (macOS 26+) actually ship <sys/vsock.h> with the full
 * vsock ABI -- AF_VSOCK, VMADDR_*, and a (BSD-packed) struct sockaddr_vm --
 * even though there is no general-purpose loopback vsock a plain process can
 * use. Prefer the real header when present so Linux-style source uses the
 * platform's own types; note that in that layout svm_port and svm_cid sit at
 * the same offsets as Linux's, so field reads are ABI-compatible either way.
 */
#if defined(__APPLE__) && defined(__has_include)
#if __has_include(<sys/vsock.h>)
#include <sys/vsock.h>
#define VSOCK_UNIX_HAVE_SYS_VSOCK 1
#endif
#endif

#ifndef AF_VSOCK
#define AF_VSOCK 40
#endif

/*
 * Fallback shim for older macOS SDKs that expose neither <sys/vsock.h> nor the
 * VMADDR_* macros. Gated on the absence of VMADDR_CID_ANY so it never clashes
 * with the platform header above (or, on Linux, with <linux/vm_sockets.h>).
 * Mirrors the classic Linux struct sockaddr_vm ABI; we only read
 * svm_family/svm_port/svm_cid to translate to a sockaddr_un.
 */
#if defined(__APPLE__) && !defined(VMADDR_CID_ANY)

struct sockaddr_vm {
    unsigned short svm_family;
    unsigned short svm_reserved1;
    unsigned int   svm_port;
    unsigned int   svm_cid;
    unsigned char  svm_zero[sizeof(struct sockaddr) -
                            sizeof(unsigned short) - sizeof(unsigned short) -
                            sizeof(unsigned int) - sizeof(unsigned int)];
};

#define VMADDR_CID_ANY        0xFFFFFFFFU
#define VMADDR_CID_HYPERVISOR 0
#define VMADDR_CID_LOCAL      1
#define VMADDR_CID_HOST       2
#define VMADDR_PORT_ANY       0xFFFFFFFFU

#endif /* __APPLE__ && !VMADDR_CID_ANY */

/* ------------------------------------------------------------------ *
 * 2. The convention + helpers.
 * ------------------------------------------------------------------ */

/*
 * Canonicalise a CID for path formation. VMADDR_CID_ANY (a wildcard bind on
 * real vsock) has no single path, so we pin it to the host CID -- the value
 * clients dial when they want "the host". Everything else is passed through.
 */
static inline unsigned int vsock_unix_canon_cid(unsigned int cid)
{
    return (cid == VMADDR_CID_ANY) ? VMADDR_CID_HOST : cid;
}

/*
 * Format the Unix-socket path for (cid,port) into buf. Returns 0 on success,
 * -1 (with errno = ENAMETOOLONG) if it would not fit.
 */
static inline int vsock_unix_path(char *buf, size_t buflen,
                                  unsigned int cid, unsigned int port)
{
    const char *dir = getenv("VSOCK_UNIX_DIR");
    char dbuf[PATH_MAX];

    if (dir == NULL || dir[0] == '\0') {
        const char *tmp = getenv("TMPDIR");
        size_t tlen;
        if (tmp == NULL || tmp[0] == '\0')
            tmp = "/tmp";
        tlen = strlen(tmp);
        /* TMPDIR on macOS commonly ends in '/'; avoid a doubled slash. */
        if (snprintf(dbuf, sizeof dbuf, "%s%svsock-unix", tmp,
                     (tlen > 0 && tmp[tlen - 1] == '/') ? "" : "/") >=
            (int)sizeof dbuf) {
            errno = ENAMETOOLONG;
            return -1;
        }
        dir = dbuf;
    }

    if (snprintf(buf, buflen, "%s/%u.%u", dir, vsock_unix_canon_cid(cid),
                 port) >= (int)buflen) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

/* mkdir -p for the directory portion of an endpoint path. Best-effort. */
static inline void vsock_unix__ensure_dir(const char *path)
{
    char tmp[PATH_MAX];
    size_t len = strlen(path);
    char *p;

    if (len >= sizeof tmp)
        return;
    memcpy(tmp, path, len + 1);

    /* Drop the final "/<cid>.<port>" component. */
    p = strrchr(tmp, '/');
    if (p == NULL || p == tmp)
        return;
    *p = '\0';

    /* Create each intermediate component. */
    for (p = tmp + 1; *p != '\0'; p++) {
        if (*p == '/') {
            *p = '\0';
            (void)mkdir(tmp, 0700);
            *p = '/';
        }
    }
    (void)mkdir(tmp, 0700);
}

/* Fill a sockaddr_un for (cid,port). Returns 0 or -1/ENAMETOOLONG. */
static inline int vsock_unix_fill_un(struct sockaddr_un *un,
                                     unsigned int cid, unsigned int port)
{
    char path[PATH_MAX];

    if (vsock_unix_path(path, sizeof path, cid, port) < 0)
        return -1;
    if (strlen(path) >= sizeof un->sun_path) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memset(un, 0, sizeof *un);
    un->sun_family = AF_UNIX;
    strcpy(un->sun_path, path);
    return 0;
}

/* Create the backing AF_UNIX stream socket. */
static inline int vsock_unix_socket(void)
{
    return socket(AF_UNIX, SOCK_STREAM, 0);
}

/*
 * Bind fd to (cid,port)'s path. Creates the parent directory and removes any
 * stale socket file first. Returns 0 or -1. (The caller issues its own
 * listen() -- the interposer relies on this so an unmodified listen() call on
 * the underlying AF_UNIX fd works untouched.)
 */
static inline int vsock_unix_bind(int fd, unsigned int cid, unsigned int port)
{
    struct sockaddr_un un;

    if (vsock_unix_fill_un(&un, cid, port) < 0)
        return -1;
    vsock_unix__ensure_dir(un.sun_path);
    (void)unlink(un.sun_path); /* clear a stale bind from a prior run */
    if (bind(fd, (struct sockaddr *)&un, (socklen_t)sizeof un) < 0)
        return -1;
    return 0;
}

/* Bind fd to (cid,port)'s path and listen. Returns 0 or -1. */
static inline int vsock_unix_bind_listen(int fd, unsigned int cid,
                                         unsigned int port, int backlog)
{
    if (vsock_unix_bind(fd, cid, port) < 0)
        return -1;
    if (listen(fd, backlog) < 0)
        return -1;
    return 0;
}

/*
 * Connect fd to (cid,port)'s path. Returns whatever connect(2) returns (0, or
 * -1 with errno -- including EINPROGRESS for a nonblocking fd, which callers
 * handle with a poll/retry loop).
 */
static inline int vsock_unix_connect(int fd, unsigned int cid,
                                     unsigned int port)
{
    struct sockaddr_un un;

    if (vsock_unix_fill_un(&un, cid, port) < 0)
        return -1;
    return connect(fd, (struct sockaddr *)&un, (socklen_t)sizeof un);
}

#endif /* VSOCK_UNIX_H */
