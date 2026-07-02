#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/sendfile.h>

#include "dw_io.h"
#include "dw_event.h"

#include <assert.h>
#include <stdbool.h>

#include "connection.h"
#include "dw_debug.h"

#ifdef IOURING_ENABLED
#include <liburing.h>
#endif

// TODO: move to dw_poll and make non static, this is used everywhere
static int is_uring(dw_poll_t *p_poll) {
    #ifdef IOURING_ENABLED
    return p_poll != NULL && p_poll->poll_type == DW_IOURING;
    #else
    assert(p_poll->poll_type != DW_IOURING);
    return false;
    #endif
}

#ifdef IOURING_ENABLED
static void uring_rearm_recv(dw_poll_t *p_poll, conn_info_t *conn, int flags) {
    dw_log("uring_rearm_recv: on conn_id : %d\n", conn_get_id_by_ptr(conn));
    struct io_uring_sqe *sqe = dw_poll_next_sqe(p_poll);
    io_uring_prep_recv(sqe, conn->sock, conn->curr_recv_buf, conn->curr_recv_size, flags);
    io_uring_sqe_set_data64(sqe, DW_URING_PACK(DW_URING_OP_RECV, conn_get_id_by_ptr(conn)));
    conn->uring_recv_in_flight = 1;
}

static void uring_rearm_accept(dw_poll_t *p_poll, conn_info_t *conn) {
    dw_log("uring_rearm_accept: on conn_id : %d\n", conn_get_id_by_ptr(conn));
    // TODO: this snippet to get an sqe should be a dw_poll exposed method together with seen etc
    struct io_uring_sqe *sqe = dw_poll_next_sqe(p_poll);
    socklen_t            len = sizeof(conn->accept);
    io_uring_prep_accept(sqe, conn->sock, (struct sockaddr *) &conn->accept, &len, 0);
    io_uring_sqe_set_data64(sqe, DW_URING_PACK(DW_URING_OP_ACCEPT, conn_get_id_by_ptr(conn)));
}
#endif

int dw_accept(dw_poll_t *p_poll, const int conn_id, struct sockaddr_in *addr, socklen_t *addrlen) {
    dw_log("dw_accept: just entered _ attemping to get connection conn_id:%d\n", conn_id);
    conn_info_t *conn = conn_get_by_id(conn_id);
    assert(conn);
    const int listen_fd = conn->sock;
    dw_log("accepting from conn_id=%d, fd=%d\n", conn_id, listen_fd);

    if (is_uring(p_poll)) {
        #ifdef IOURING_ENABLED
        struct io_uring_cqe *cqe = dw_poll_current_cqe(p_poll);
        if (!cqe) {
            errno = EAGAIN;
            return -1;
        }
        int res = cqe->res;
        dw_poll_cqe_seen(p_poll, cqe);

        if (addr || addrlen) {
            assert(addr && addrlen);
            memcpy(addr, &conn->accept, sizeof(conn->accept));
            *addrlen = sizeof(conn->accept);
        }

        uring_rearm_accept(p_poll, conn);
        if (res < 0) {
            errno = -res;
            return -1;
        }
        return res; // new fd
        #endif
    }

    return accept(listen_fd, (struct sockaddr *) addr, addrlen);
}

ssize_t dw_sendto(dw_poll_t *p_poll, const int conn_id, const int flags) {
    dw_log("dw_sendto: just entered\n");
    conn_info_t *conn = conn_get_by_id(conn_id);
    assert(conn);

    if (is_uring(p_poll)) {
        #ifdef IOURING_ENABLED
        dw_log("dw_sendto: IOURING_ENABLED: conn->uring_send_state:%d; remember READY:0 ; IN_FLIGHT:1 ; COMPLETED:2\n", conn->uring_send_state);
        switch (conn->uring_send_state) {
            case SS_READY: {
                // means previous completion (if any) has been computed, we ready for a new prep and submit.
                //  after submission it will be IN_FLIGHT
                struct io_uring_sqe *sqe = dw_poll_next_sqe(p_poll);
                // MSG_WAITALL guarantees the entire passed buffer is written before returning a CQE
                io_uring_prep_sendto(sqe, conn->sock, conn->curr_send_buf, conn->curr_send_size,
                                     flags | MSG_WAITALL, (struct sockaddr *) &conn->target, sizeof(conn->target));
                io_uring_sqe_set_data64(sqe, DW_URING_PACK(DW_URING_OP_SEND, conn_id));
                conn->uring_send_state = SS_IN_FLIGHT;
                errno                  = EAGAIN;

                dw_log("dw_sendto: -> SEND_URING: conn_id=%d SQE was submitted, setting state SS_IN_FLIGHT\n", conn_id);
                return -1;
            }
            case SS_IN_FLIGHT: {
                check(0, "dw_sendto: should not call dw_sendto while in flight");
            }
            case SS_COMPLETED: {
                // complete: consume the SEND CQE the dispatcher just emitted
                dw_log("dw_sendto: case SS_COMPLETED, we shall consume the send-CQE!!! ---\n");
                struct io_uring_cqe *cqe = dw_poll_current_cqe(p_poll);
                if (!cqe || DW_URING_UNPACK_OP(io_uring_cqe_get_data64(cqe)) != DW_URING_OP_SEND) {
                    errno = EINVAL;
                    return -1;
                }
                int res = cqe->res;
                dw_poll_cqe_seen(p_poll, cqe);
                if (res < 0) {
                    errno = -res;
                    return -1;
                }

                conn->uring_send_state = SS_READY;
                dw_log("dw_sendto: -> SEND_URING: conn_id=%d is now ready, setting state SS_REDY\n", conn_id);

                // now that the send has been completed, we shall pass through the reply -> conn_req_remove way.
                // cannot access req after conn_req_remove()
                dw_log("dw_sendto: send has completed, we might need to call conn_req_remove\n");

                return res;
            };
        }
        #endif
    }

    dw_log("dw_sendto: standard path-> calling sendto\n");
    return sendto(conn->sock, conn->curr_send_buf, conn->curr_send_size, flags,
                  (struct sockaddr *) &conn->target, sizeof(conn->target));
}

ssize_t dw_recvfrom(dw_poll_t *p_poll, const int conn_id, const int flags, struct sockaddr_in *from, socklen_t *from_len) {
    conn_info_t *conn = conn_get_by_id(conn_id);
    assert(conn);
    dw_log("dw_recvfrom: just entered\n");

    if (conn->req_list){
        dw_log("dw_recvfrom just entered: --- currently, conn.req_list is not null\n");
    }else{
        dw_log("dw_recvfrom just entered: --- currently, conn.req_list is NULL\n");
    }

    if (is_uring(p_poll)) {
        #ifdef IOURING_ENABLED
        struct io_uring_cqe *cqe = dw_poll_current_cqe(p_poll);
        if (!cqe) {
            errno = EAGAIN;
            return -1;
        }
        int res = cqe->res;
        dw_poll_cqe_seen(p_poll, cqe);

        conn->uring_recv_in_flight = 0;

        if (res < 0) {
            errno = -res;
        }

        if (conn->defer_defrag > 0) {
            if (res > 0) {
                dw_log("DEFER_DEFRAG (IOURING_ENABLED): moved new data backwards by %lu\n", conn->defer_defrag);
                memmove(conn->curr_recv_buf, conn->curr_recv_buf + conn->defer_defrag, res);
            }
            conn->defer_defrag = 0;
        }

        if (res != 0) {
            conn->curr_recv_buf += res;
            conn->curr_recv_size -= res;
            uring_rearm_recv(p_poll, conn, flags);
        }

        // peer addr isn't filled by recv, zero if requested
        // TODO: switch to io_uring_prep_recvfrom which actually exposes this but needs more machinery to get it out

        return res;
        #endif
    }

    const ssize_t res = recvfrom(conn->sock, conn->curr_recv_buf, conn->curr_recv_size, flags, (struct sockaddr *) from, from_len);
    conn->curr_recv_buf += res;
    conn->curr_recv_size -= res;
    return res;
}

#ifdef IOURING_ENABLED
const size_t BUF_LEN   = 64 * 1024;
const size_t BUF_ALIGN = 4096;

static int uring_init_sendfile_buffers(dw_poll_t *p_poll) {
    if (p_poll->u.iouring_fds.sendfile_bufs_registered) return 0;
    io_uring_register_buffers_sparse(&p_poll->u.iouring_fds.ring, MAX_POLLFD);
    p_poll->u.iouring_fds.sendfile_bufs_registered = true;
    return 0;
}

static int uring_prepare_sendfile_buf(dw_poll_t *const p_poll, conn_info_t *const conn) {
    if (uring_init_sendfile_buffers(p_poll) < 0) return -1;

    void *buf = NULL;
    if (posix_memalign(&buf, BUF_ALIGN, BUF_LEN) != 0) {
        errno = ENOMEM;
        return -1;
    }
    struct iovec iov = {
        .iov_base = buf,
        .iov_len  = BUF_LEN,
    };
    int rv = io_uring_register_buffers_update_tag(&p_poll->u.iouring_fds.ring, conn_get_id_by_ptr(conn), &iov, NULL, 1);
    if (rv < 0) {
        free(buf);
        errno = -rv;
        return -1;
    }
    conn->uring_sendfile_scratch = buf;

    return 0;
}
#endif

ssize_t dw_sendfile(dw_poll_t *p_poll, const int conn_id) {
    conn_info_t *conn = conn_get_by_id(conn_id);
    assert(conn);

    if (is_uring(p_poll)) {
        #ifdef IOURING_ENABLED
        if (conn->uring_send_state == SS_IN_FLIGHT) {
            check(0, "should not call dw_sendto while in flight");
        }

        if (conn->uring_send_state == SS_COMPLETED) {
            struct io_uring_cqe *cqe = dw_poll_current_cqe(p_poll);
            assert(cqe);

            int res = cqe->res;
            dw_poll_cqe_seen(p_poll, cqe);
            if (res < 0) {
                errno = -res;
                return -1;
            }

            if (conn->curr_send_size == 0 && conn->file_remaining == 0) {
                return 0;
            }
        }

        if (uring_prepare_sendfile_buf(p_poll, conn) < 0) return -1;

        int sqes_needed = (conn->curr_send_size > 0 ? 1 : 0) + 2;

        struct io_uring *r = &p_poll->u.iouring_fds.ring;
        if (io_uring_sq_space_left(r) < sqes_needed) {
            io_uring_submit(r);
            if (io_uring_sq_space_left(r) < sqes_needed) {
                errno = EAGAIN;
                return -1;
            }
        }

        if (conn->uring_send_state == SS_READY && conn->curr_send_size > 0) {
            struct io_uring_sqe *sqe = io_uring_get_sqe(r);
            assert(sqe);
            io_uring_prep_send(sqe, conn->sock, conn->curr_send_buf, conn->curr_send_size, MSG_NOSIGNAL | MSG_MORE | MSG_WAITALL);
            sqe->flags |= IOSQE_IO_LINK | IOSQE_CQE_SKIP_SUCCESS;
            io_uring_sqe_set_data64(sqe, DW_URING_PACK(DW_URING_OP_SENDFILE_FAILED, conn_id));
        }

        // TODO: what happens on a partial read?

        size_t to_read = conn->file_remaining > BUF_LEN ? BUF_LEN : conn->file_remaining;

        struct io_uring_sqe *s_read = io_uring_get_sqe(r);
        assert(s_read);
        io_uring_prep_read_fixed(s_read, conn->file_fd, conn->uring_sendfile_scratch, to_read, 0, conn_id);
        // conn_id is _also_ the buffer index in the register ones
        s_read->flags |= IOSQE_IO_LINK | IOSQE_CQE_SKIP_SUCCESS;
        io_uring_sqe_set_data64(s_read, DW_URING_PACK(DW_URING_OP_SENDFILE_FAILED, conn_id));

        struct io_uring_sqe *s_send = io_uring_get_sqe(r);
        assert(s_send);
        io_uring_prep_send_zc_fixed(s_send, conn->sock, conn->uring_sendfile_scratch, to_read, MSG_NOSIGNAL | MSG_WAITALL, 0, conn_id);
        // conn_id is _also_ the buffer index in the register ones
        io_uring_sqe_set_data64(s_send, DW_URING_PACK(DW_URING_OP_SENDFILE, conn_id));

        dw_log("--- SENDFILE_URING: prepped %d SQE\n", sqes_needed);
        conn->uring_send_state = SS_IN_FLIGHT;

        return to_read;
        #endif
    }

    // PHASE 1: Send the header (blocking until header is fully sent, then move on to sendfile)
    while (conn->curr_send_size > 0) {
        // Use MSG_MORE to tell the kernel: "Don't flush the packet yet, file data is coming!"
        ssize_t sent = send(conn->sock, conn->curr_send_buf, conn->curr_send_size, MSG_NOSIGNAL | MSG_MORE);

        if (sent <= 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue; // try again until we can send the header
            if (errno == EPIPE || errno == ECONNRESET) {
                return -1;
            }
        }

        conn->curr_send_buf  += sent;
        conn->curr_send_size -= sent;
    }

    // PHASE 2
    return sendfile(conn->sock, conn->file_fd, &conn->file_offset, conn->file_remaining);
}
