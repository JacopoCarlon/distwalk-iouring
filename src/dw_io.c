#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>

#include "dw_io.h"
#include "dw_event.h"

#include <assert.h>
#include <stdbool.h>

#include "dw_debug.h"

#ifdef IOURING_ENABLED
#include <liburing.h>
#endif

// TODO: move to dw_poll and make non static, this is used everywhere
static int is_uring(dw_poll_t *p_poll) {
#ifdef IOURING_ENABLED
    return p_poll != NULL && p_poll->poll_type == DW_IOURING;
#else
    return false;
#endif
}

#ifdef IOURING_ENABLED
static void uring_rearm_recv(dw_poll_t *p_poll, int sock, uint64_t aux) {
    dw_poll_fd_buf_t *e = dw_poll_get_fd_buf(p_poll, sock);
    assert(e && e->buffer);
    assert(e->write_off < e->buffer_len);

    struct io_uring_sqe *sqe = io_uring_get_sqe(&p_poll->u.iouring_fds.ring);
    if (!sqe) {
        io_uring_submit(&p_poll->u.iouring_fds.ring);
        sqe = io_uring_get_sqe(&p_poll->u.iouring_fds.ring);
        assert(sqe);
    }
    io_uring_prep_recv(sqe, sock, e->buffer + e->write_off, e->buffer_len - e->write_off, 0);
    io_uring_sqe_set_data64(sqe, DW_URING_PACK(DW_URING_OP_RECV, aux));
    e->last_off = e->write_off;
}

static void uring_rearm_accept(dw_poll_t *p_poll, int listen_fd, uint64_t aux) {
    // TODO: this snippet to get an sqe should be a dw_poll exposed method together with seen etc
    struct io_uring_sqe *sqe = io_uring_get_sqe(&p_poll->u.iouring_fds.ring);
    if (!sqe) {
        io_uring_submit(&p_poll->u.iouring_fds.ring);
        sqe = io_uring_get_sqe(&p_poll->u.iouring_fds.ring);
        if (!sqe)
            return;
    }
    dw_poll_fd_buf_t *e = dw_poll_get_fd_buf(p_poll, listen_fd);
    if (e) {
        e->accept_addrlen = sizeof(e->accept_addr);
        io_uring_prep_accept(sqe, listen_fd,
                             (struct sockaddr *) &e->accept_addr,
                             &e->accept_addrlen, 0);
    } else {
        io_uring_prep_accept(sqe, listen_fd, NULL, NULL, 0);
    }
    dw_log("ACCEPT_REARM aux=%ld\n", aux);
    io_uring_sqe_set_data64(sqe, DW_URING_PACK(DW_URING_OP_ACCEPT, aux));
}
#endif

int dw_accept(dw_poll_t *p_poll, int listen_fd,
              struct sockaddr_in *addr, socklen_t *addrlen) {
    #ifdef IOURING_ENABLED
    if (is_uring(p_poll)) {
        struct io_uring_cqe *cqe = dw_poll_current_cqe(p_poll);
        if (!cqe) {
            errno = EAGAIN;
            return -1;
        }
        int res = cqe->res;
        uint64_t aux = DW_URING_UNPACK_AUX(io_uring_cqe_get_data64(cqe));
        dw_poll_cqe_seen(p_poll, cqe);
        // peer addr was filled into the per-listen-fd slot when the SQE was armed;
        // copy it out before re-arming overwrites the slot.
        dw_poll_fd_buf_t *e = dw_poll_get_fd_buf(p_poll, listen_fd);
        if (addr) {
            if (e)
                memcpy(addr, &e->accept_addr, sizeof(*addr));
            else
                memset(addr, 0, sizeof(*addr));
        }
        if (addrlen)
            *addrlen = e ? e->accept_addrlen : 0;
        uring_rearm_accept(p_poll, listen_fd, aux);
        if (res < 0) {
            errno = -res;
            return -1;
        }
        return res; // new fd
    }
    #endif

    return accept(listen_fd, (struct sockaddr *) addr, addrlen);
}

ssize_t dw_sendto(dw_poll_t *p_poll, int sock, int part, uint64_t aux,
                  const void *buf, size_t len, int flags,
                  const struct sockaddr *dest, socklen_t dest_len) {
    #ifdef IOURING_ENABLED
    if (is_uring(p_poll)) {
        if (part == 0) {
            struct io_uring_sqe *sqe = io_uring_get_sqe(&p_poll->u.iouring_fds.ring);
            if (!sqe) {
                io_uring_submit(&p_poll->u.iouring_fds.ring);
                sqe = io_uring_get_sqe(&p_poll->u.iouring_fds.ring);
                if (!sqe) {
                    errno = EAGAIN;
                    return -1;
                }
            }
            // MSG_WAITALL guarantees the entire passed buffer is written before returning a CQE
            io_uring_prep_sendto(sqe, sock, buf, len, flags | MSG_WAITALL, dest, dest_len);
            io_uring_sqe_set_data64(sqe, DW_URING_PACK(DW_URING_OP_SEND, aux));
            errno = EAGAIN;
            return -1;
        }

        if (part == 1) {
            // complete: consume the SEND CQE the dispatcher just emitted
            struct io_uring_cqe *cqe = dw_poll_current_cqe(p_poll);
            if (!cqe || DW_URING_UNPACK_OP(io_uring_cqe_get_data64(cqe)) != DW_URING_OP_SEND) {
                errno = EAGAIN;
                return -1;
            }
            int res = cqe->res;
            dw_poll_cqe_seen(p_poll, cqe);
            if (res < 0) {
                errno = -res;
                return -1;
            }
            return res;
        }

        errno = EINVAL;
        return -1;
    }
    #endif

    return sendto(sock, buf, len, flags, dest, dest_len);
}

ssize_t dw_recvfrom(dw_poll_t *p_poll, int sock,
                    void **out_buf, int flags,
                    struct sockaddr *from, socklen_t *from_len) {
    dw_poll_fd_buf_t *e = p_poll ? dw_poll_get_fd_buf(p_poll, sock) : NULL;
    if (!e || e->buffer == NULL) {
        // no registered buffer, hopefully unreachable
        // TODO: dw_log + maybe hard crash
        errno = EINVAL;
        return -1;
    }
    if (e->write_off >= e->buffer_len) {
        // buffer full, the caller must drain (consume + dw_poll_set_buffer_offset)
        errno = ENOBUFS;
        return -1;
    }

    #ifdef IOURING_ENABLED
    if (is_uring(p_poll)) {
        struct io_uring_cqe *cqe = dw_poll_current_cqe(p_poll);
        if (!cqe) {
            errno = EAGAIN;
            return -1;
        }
        int res = cqe->res;
        uint64_t aux = DW_URING_UNPACK_AUX(io_uring_cqe_get_data64(cqe));
        dw_poll_cqe_seen(p_poll, cqe);

        if (res < 0) {
            // re-arm even on error so the conn stays watched (unless cancelled which should not even get here, filtered in dw_poll_next)
            if (res != -ECANCELED)
                uring_rearm_recv(p_poll, sock, aux);
            errno = -res;
            return -1;
        }

        if (res == 0) {
            // remote close -> don't re-arm
            if (out_buf)
                *out_buf = e->buffer + e->write_off;
            return 0;
        }

        // deferred defrag if offset changed
        if (e->write_off != e->last_off) {
            memmove(e->buffer + e->write_off, e->buffer + e->last_off, res);
        }

        if (out_buf) {
            *out_buf = e->buffer + e->write_off;
        }

        // peer addr isn't filled by recv, zero if requested
        // TODO: switch to io_uring_prep_recvmsg which actually exposes this but needs more machinery to get it out
        if (from)
            memset(from, 0, sizeof(struct sockaddr_in));
        if (from_len)
            *from_len = 0;

        e->write_off += (size_t) res;
        uring_rearm_recv(p_poll, sock, aux);

        return res;
    }
    #endif

    char *head = e->buffer + e->write_off;
    size_t space = e->buffer_len - e->write_off;
    ssize_t n = recvfrom(sock, head, space, flags, from, from_len);
    if (n > 0) {
        if (out_buf)
            *out_buf = head;
        e->write_off += (size_t) n;
    }
    return n;
}

#ifdef IOURING_ENABLED
static int uring_init_sendfile_buffers(dw_poll_t *p_poll) {
    if (p_poll->u.iouring_fds.sendfile_bufs_registered) return 0;
    io_uring_register_buffers_sparse(&p_poll->u.iouring_fds.ring, MAX_POLLFD);
    p_poll->u.iouring_fds.sendfile_bufs_registered = true;
    return 0;
}

const size_t BUF_LEN = 64 * 1024;
const size_t BUF_ALIGN = 4096;

static int uring_prepare_sendfile_buf(dw_poll_t *p_poll, dw_poll_fd_buf_t *e) {
    if (e->sendfile_buf != NULL) return 0;
    if (uring_init_sendfile_buffers(p_poll) < 0) return -1;

    void *buf = NULL;
    if (posix_memalign(&buf, BUF_ALIGN, BUF_LEN) != 0) {
        errno = ENOMEM;
        return -1;
    }
    struct iovec iov = {
        .iov_base = buf,
        .iov_len = BUF_LEN,
    };
    int rv = io_uring_register_buffers_update_tag(&p_poll->u.iouring_fds.ring, e->sendfile_buf_index, &iov, NULL, 1);
    if (rv < 0) {
        free(buf);
        errno = -rv;
        return -1;
    }
    e->sendfile_buf = buf;
    e->sendfile_buf_len = BUF_LEN;
    return 0;
}
#endif

ssize_t dw_sendfile(dw_poll_t *p_poll, int sock, int part, uint64_t aux,
                    const void *hdr, size_t hdr_len,
                    int fd_in, off_t off_in, size_t len) {
    #ifdef IOURING_ENABLED
    if (is_uring(p_poll)) {
        if (part == 1)  {
            struct io_uring_cqe *cqe = dw_poll_current_cqe(p_poll);
            int res = cqe->res;
            dw_poll_cqe_seen(p_poll, cqe);
            if (res < 0) {
                errno = -res;
                return -1;
            }

            if (hdr_len == 0 && len == 0) {
                return 0;
            }
        }

        dw_poll_fd_buf_t *e = dw_poll_get_fd_buf(p_poll, sock);
        assert(e);

        if (uring_prepare_sendfile_buf(p_poll, e) < 0)
            return -1;

        size_t to_read = len;
        if (to_read > e->sendfile_buf_len)
            to_read = e->sendfile_buf_len;
        if (to_read == 0) {
            errno = EINVAL;
            return -1;
        }

        int sqes_needed = (hdr_len > 0 ? 1 : 0) + 2;
        struct io_uring *r = &p_poll->u.iouring_fds.ring;
        if (io_uring_sq_space_left(r) < sqes_needed) {
            io_uring_submit(r);
            if (io_uring_sq_space_left(r) < sqes_needed) {
                errno = EAGAIN;
                return -1;
            }
        }

        if (part == 0 && hdr_len > 0) {
            struct io_uring_sqe *sqe = io_uring_get_sqe(r);
            assert(sqe);
            io_uring_prep_send(sqe, sock, hdr, hdr_len, MSG_NOSIGNAL | MSG_MORE | MSG_WAITALL);
            sqe->flags |= IOSQE_IO_LINK | IOSQE_CQE_SKIP_SUCCESS;
            io_uring_sqe_set_data64(sqe, DW_URING_PACK(DW_URING_OP_SENDFILE_FAILED, aux));
        }

        struct io_uring_sqe *s_read = io_uring_get_sqe(r);
        assert(s_read);
        io_uring_prep_read_fixed(s_read, fd_in, e->sendfile_buf, to_read, off_in, e->sendfile_buf_index);
        s_read->flags |= IOSQE_IO_LINK | IOSQE_CQE_SKIP_SUCCESS;
        io_uring_sqe_set_data64(s_read, DW_URING_PACK(DW_URING_OP_SENDFILE_FAILED, aux));

        struct io_uring_sqe *s_send = io_uring_get_sqe(r);
        assert(s_send);
        io_uring_prep_send_zc(s_send, sock, e->sendfile_buf, to_read, MSG_NOSIGNAL | MSG_WAITALL, 0);
        io_uring_sqe_set_data64(s_send, DW_URING_PACK(DW_URING_OP_SENDFILE, aux));

        dw_log("--- SENDFILE_URING: prepped %d SQE\n", sqes_needed);

        return to_read;
    }
    #endif
    errno = ENOSYS;
    return -1;
}
