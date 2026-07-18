/* SPDX-License-Identifier: ISC */
/* Copyright (c) 2026 Gil Bahat */
/*
 * interpose.c -> libvsock_unix.dylib
 *
 * A DYLD interposer that makes unmodified, dynamically-linked libc programs
 * using Linux-style AF_VSOCK "just work" on macOS, by backing every AF_VSOCK
 * socket with an AF_UNIX socket and translating struct sockaddr_vm <->
 * struct sockaddr_un through the shared (cid,port)->path convention in
 * vsock_unix.h.
 *
 * Load it with:
 *     DYLD_INSERT_LIBRARIES=/path/to/libvsock_unix.dylib ./your-program
 *
 * We interpose socket/bind/connect/accept/getsockname/getpeername/close.
 * listen(), read()/write()/send()/recv() need no interposition -- they act on
 * the underlying AF_UNIX fd unchanged. A small fd table records which fds we
 * own and their (cid,port) so getsockname/getpeername can synthesise a
 * sockaddr_vm back to the caller.
 *
 * dyld does not apply interposition to the image that registers it, so the
 * plain socket()/bind()/connect()/... calls made below (directly and via the
 * vsock_unix_* helpers) reach the real C library, not back into us.
 */

#include "vsock_unix.h"

#include <pthread.h>

/* ------------------------------------------------------------------ *
 * fd table
 * ------------------------------------------------------------------ */

/*
 * Indexed by fd. macOS fds are small dense ints; anything at or above this cap
 * is simply left untracked (treated as a normal, non-vsock fd), which fails
 * safe. 64K covers any realistic RLIMIT_NOFILE.
 */
#define VSU_MAX_FDS 65536

struct vsu_entry {
    unsigned char is_vsock;  /* this fd is one we back with AF_UNIX */
    unsigned char bound;     /* a local (cid,port) was set via bind() */
    unsigned char connected; /* a remote (cid,port) was set via connect() */
    unsigned int  cid, port;           /* local endpoint */
    unsigned int  peer_cid, peer_port; /* remote endpoint (connect/accept) */
};

static struct vsu_entry vsu_tab[VSU_MAX_FDS];
static pthread_mutex_t vsu_lock = PTHREAD_MUTEX_INITIALIZER;

static void vsu_clear(int fd)
{
    if (fd < 0 || fd >= VSU_MAX_FDS)
        return;
    pthread_mutex_lock(&vsu_lock);
    memset(&vsu_tab[fd], 0, sizeof vsu_tab[fd]);
    pthread_mutex_unlock(&vsu_lock);
}

static int vsu_is_vsock(int fd)
{
    int r;
    if (fd < 0 || fd >= VSU_MAX_FDS)
        return 0;
    pthread_mutex_lock(&vsu_lock);
    r = vsu_tab[fd].is_vsock;
    pthread_mutex_unlock(&vsu_lock);
    return r;
}

/* ------------------------------------------------------------------ *
 * sockaddr_vm helpers
 * ------------------------------------------------------------------ */

/* Fill *out (a caller buffer of *outlen bytes) with a synthetic sockaddr_vm. */
static void vsu_fill_vm(struct sockaddr *out, socklen_t *outlen,
                        unsigned int cid, unsigned int port)
{
    struct sockaddr_vm vm;

    if (out == NULL || outlen == NULL)
        return;
    memset(&vm, 0, sizeof vm);
    vm.svm_family = AF_VSOCK;
    vm.svm_port = port;
    vm.svm_cid = cid;

    if (*outlen > (socklen_t)sizeof vm)
        *outlen = (socklen_t)sizeof vm;
    memcpy(out, &vm, *outlen);
    *outlen = (socklen_t)sizeof vm; /* report the full size, like the kernel */
}

/* ------------------------------------------------------------------ *
 * interposed calls
 * ------------------------------------------------------------------ */

static int vsu_socket(int domain, int type, int protocol)
{
    int fd;

    if (domain != AF_VSOCK)
        return socket(domain, type, protocol);

    /* Back it with a Unix stream socket. vsock is stream-oriented; drop any
     * Linux-only type flags (SOCK_NONBLOCK/CLOEXEC don't exist on macOS). */
    (void)type;
    (void)protocol;
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    if (fd >= VSU_MAX_FDS)
        return fd; /* untracked; behaves as a plain Unix socket */

    pthread_mutex_lock(&vsu_lock);
    memset(&vsu_tab[fd], 0, sizeof vsu_tab[fd]);
    vsu_tab[fd].is_vsock = 1;
    pthread_mutex_unlock(&vsu_lock);
    return fd;
}

static int vsu_bind(int fd, const struct sockaddr *addr, socklen_t len)
{
    const struct sockaddr_vm *vm;
    unsigned int cid, port;

    if (!vsu_is_vsock(fd) || addr == NULL || len < (socklen_t)sizeof(*vm))
        return bind(fd, addr, len);

    vm = (const struct sockaddr_vm *)addr;
    cid = vm->svm_cid;
    port = vm->svm_port;

    if (vsock_unix_bind(fd, cid, port) < 0)
        return -1;

    pthread_mutex_lock(&vsu_lock);
    vsu_tab[fd].bound = 1;
    vsu_tab[fd].cid = vsock_unix_canon_cid(cid);
    vsu_tab[fd].port = port;
    pthread_mutex_unlock(&vsu_lock);
    return 0;
}

static int vsu_connect(int fd, const struct sockaddr *addr, socklen_t len)
{
    const struct sockaddr_vm *vm;
    unsigned int cid, port;
    int rc;

    if (!vsu_is_vsock(fd) || addr == NULL || len < (socklen_t)sizeof(*vm))
        return connect(fd, addr, len);

    vm = (const struct sockaddr_vm *)addr;
    cid = vm->svm_cid;
    port = vm->svm_port;

    rc = vsock_unix_connect(fd, cid, port);

    /* Record the target even on EINPROGRESS (nonblocking connect still in
     * flight): the caller will poll for completion and may query getpeername. */
    if (rc == 0 || errno == EINPROGRESS || errno == EALREADY ||
        errno == EISCONN) {
        pthread_mutex_lock(&vsu_lock);
        vsu_tab[fd].connected = 1;
        vsu_tab[fd].peer_cid = vsock_unix_canon_cid(cid);
        vsu_tab[fd].peer_port = port;
        pthread_mutex_unlock(&vsu_lock);
    }
    return rc;
}

static int vsu_accept(int fd, struct sockaddr *addr, socklen_t *addrlen)
{
    int cfd;
    unsigned int local_cid, local_port;

    if (!vsu_is_vsock(fd)) {
        return accept(fd, addr, addrlen);
    }

    /* Accept on the underlying Unix socket; ignore its (empty) sockaddr_un. */
    cfd = accept(fd, NULL, NULL);
    if (cfd < 0)
        return -1;

    pthread_mutex_lock(&vsu_lock);
    local_cid = vsu_tab[fd].cid;
    local_port = vsu_tab[fd].port;
    pthread_mutex_unlock(&vsu_lock);

    if (cfd < VSU_MAX_FDS) {
        pthread_mutex_lock(&vsu_lock);
        memset(&vsu_tab[cfd], 0, sizeof vsu_tab[cfd]);
        vsu_tab[cfd].is_vsock = 1;
        vsu_tab[cfd].connected = 1;
        /*
         * We cannot recover the connector's CID over AF_UNIX, so peer info is
         * best-effort: report the host CID and an unknown (0) port. Consumers
         * that only read/write the accepted fd (the common case) don't care.
         */
        vsu_tab[cfd].peer_cid = VMADDR_CID_HOST;
        vsu_tab[cfd].peer_port = 0;
        vsu_tab[cfd].cid = local_cid;
        vsu_tab[cfd].port = local_port;
        pthread_mutex_unlock(&vsu_lock);
    }

    /* Hand back a synthetic peer sockaddr_vm if the caller asked for one. */
    if (addr != NULL && addrlen != NULL)
        vsu_fill_vm(addr, addrlen, VMADDR_CID_HOST, 0);
    return cfd;
}

static int vsu_getsockname(int fd, struct sockaddr *addr, socklen_t *addrlen)
{
    unsigned int cid, port;

    if (!vsu_is_vsock(fd))
        return getsockname(fd, addr, addrlen);

    pthread_mutex_lock(&vsu_lock);
    cid = vsu_tab[fd].bound ? vsu_tab[fd].cid : VMADDR_CID_ANY;
    port = vsu_tab[fd].bound ? vsu_tab[fd].port : VMADDR_PORT_ANY;
    pthread_mutex_unlock(&vsu_lock);

    vsu_fill_vm(addr, addrlen, cid, port);
    return 0;
}

static int vsu_getpeername(int fd, struct sockaddr *addr, socklen_t *addrlen)
{
    unsigned int cid, port;
    int connected;

    if (!vsu_is_vsock(fd))
        return getpeername(fd, addr, addrlen);

    pthread_mutex_lock(&vsu_lock);
    connected = vsu_tab[fd].connected;
    cid = vsu_tab[fd].peer_cid;
    port = vsu_tab[fd].peer_port;
    pthread_mutex_unlock(&vsu_lock);

    if (!connected) {
        errno = ENOTCONN;
        return -1;
    }
    vsu_fill_vm(addr, addrlen, cid, port);
    return 0;
}

static int vsu_close(int fd)
{
    /* Drop our record before the fd number can be recycled. */
    vsu_clear(fd);
    return close(fd);
}

/* ------------------------------------------------------------------ *
 * interpose table (__DATA,__interpose)
 * ------------------------------------------------------------------ */

#define VSU_INTERPOSE(_new, _old)                                            \
    __attribute__((used)) static struct {                                    \
        const void *nw;                                                      \
        const void *old;                                                     \
    } _vsu_i_##_old __attribute__((section("__DATA,__interpose"))) = {       \
        (const void *)(unsigned long)&_new, (const void *)(unsigned long)&_old}

VSU_INTERPOSE(vsu_socket, socket);
VSU_INTERPOSE(vsu_bind, bind);
VSU_INTERPOSE(vsu_connect, connect);
VSU_INTERPOSE(vsu_accept, accept);
VSU_INTERPOSE(vsu_getsockname, getsockname);
VSU_INTERPOSE(vsu_getpeername, getpeername);
VSU_INTERPOSE(vsu_close, close);
