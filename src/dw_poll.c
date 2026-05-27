#include <stddef.h>
#include <sys/socket.h>
#include <sys/uio.h>

#include "dw_poll.h"

#include <assert.h>

#include "dw_debug.h"
#include "dw_event.h"

#include "connection.h"

// fd associated buffers are now handled
dw_poll_fd_buf_t *dw_poll_get_fd_buf(dw_poll_t *p_poll, int fd) {
    for (int i = 0; i < p_poll->n_fd_bufs; i++)
        if (p_poll->fd_bufs[i].fd == fd)
            return &p_poll->fd_bufs[i];
    return NULL;
}

static void dw_poll_remove_fd_buf(dw_poll_t *p_poll, int fd) {
    for (int i = 0; i < p_poll->n_fd_bufs; i++) {
        if (p_poll->fd_bufs[i].fd == fd) {
            #ifdef IOURING_ENABLED
            if (p_poll->poll_type == DW_IOURING && p_poll->fd_bufs[i].sendfile_buf) {
                struct iovec empty_iov = { .iov_base = NULL, .iov_len = 0 };
                check(io_uring_register_buffers_update_tag(&p_poll->u.iouring_fds.ring, p_poll->fd_bufs[i].sendfile_buf_index, &empty_iov, NULL, 1), "no\n");
                free(p_poll->fd_bufs[i].sendfile_buf);
                p_poll->fd_bufs[i].sendfile_buf = NULL;
                p_poll->fd_bufs[i].sendfile_buf_len = 0;
            }
            #endif

            // swaps last entry with the one just removed to keep compact
            p_poll->n_fd_bufs--;
            if (i < p_poll->n_fd_bufs)
                p_poll->fd_bufs[i] = p_poll->fd_bufs[p_poll->n_fd_bufs];
            return;
        }
    }
}

int dw_poll_set_buffer_offset(dw_poll_t *p_poll, int fd, size_t new_off) {
    dw_poll_fd_buf_t *e = dw_poll_get_fd_buf(p_poll, fd);
    if (!e)
        return -1;
    e->write_off = new_off;
    return 0;
}

// return value useful to return failure if we allocate memory here in the future
int dw_poll_init(dw_poll_t *p_poll, dw_poll_type_t type, int use_spinning) {
    p_poll->poll_type = type;
    p_poll->use_spinning = use_spinning;
    p_poll->n_fd_bufs = 0;
    switch (p_poll->poll_type) {
        case DW_SELECT:
            p_poll->u.select_fds.n_rd_fd = 0;
            p_poll->u.select_fds.n_wr_fd = 0;
            break;
        case DW_POLL:
            p_poll->u.poll_fds.n_pollfds = 0;
            break;
        case DW_EPOLL:
            sys_check(p_poll->u.epoll_fds.epollfd = epoll_create1(0));
            break;
        case DW_IOURING:
            #ifdef IOURING_ENABLED
            int rv = io_uring_queue_init(MAX_POLLFD, &p_poll->u.iouring_fds.ring, IORING_SETUP_SQPOLL);
            if (rv < 0) {
                errno = -rv;
                sys_check(rv);
            }
            p_poll->u.iouring_fds.n_cqes = 0;
            p_poll->u.iouring_fds.iter = -1;
            p_poll->u.iouring_fds.cqe_batch_limit = 4;
            p_poll->u.iouring_fds.consumed_since_submit = 0;

            for (unsigned int i = 0; i < MAX_POLLFD; i++) {
                p_poll->fd_bufs[i].sendfile_buf_index = i;
            }
            #else
            check(0, "DW_IOURING requested but build lacks IOURING_ENABLED");
            #endif
            break;
        default:
            check(0, "Wrong dw_poll_type");
    }
    return 0;
}

#ifdef IOURING_ENABLED
static int dw_uring_arm_pollin(dw_poll_t *p_poll, int fd, dw_poll_flags flags, uint64_t aux) {
    assert(flags & DW_POLLIN);

    struct io_uring_sqe *sqe = io_uring_get_sqe(&p_poll->u.iouring_fds.ring);
    if (!sqe) {
        // if the SQ full, enter and retry
        io_uring_submit(&p_poll->u.iouring_fds.ring);
        sqe = io_uring_get_sqe(&p_poll->u.iouring_fds.ring);
        if (!sqe) {
            errno = EAGAIN;
            return -1;
        }
    }

    // prepare an accept
    if (flags & DW_ACCEPT) {
        dw_poll_fd_buf_t *e = dw_poll_get_fd_buf(p_poll, fd);
        if (e) {
            e->accept_addrlen = sizeof(e->accept_addr);
            io_uring_prep_accept(sqe, fd,
                                 (struct sockaddr *) &e->accept_addr,
                                 &e->accept_addrlen, 0);
        } else {
            io_uring_prep_accept(sqe, fd, NULL, NULL, 0);
        }
        io_uring_sqe_set_data64(sqe, DW_URING_PACK(DW_URING_OP_ACCEPT, aux));
    }
    // prepare a generic recv/poll depending on whether a buffer was passed to dw_poll_add
    // TODO: we will need another way to differentiate if we ever need to support disk IO
    else {
        dw_poll_fd_buf_t *e = dw_poll_get_fd_buf(p_poll, fd);
        if (e && e->buffer) {
            // oneshot recv into the registered buffer at the current write head
            io_uring_prep_recv(sqe, fd, e->buffer + e->write_off, e->buffer_len - e->write_off, 0);
            io_uring_sqe_set_data64(sqe, DW_URING_PACK(DW_URING_OP_RECV, aux));
            e->last_off = e->write_off;
        } else {
            // readiness only if no registered buffer used for timerfd, eventfd, signalfd, ...
            io_uring_prep_poll_add(sqe, fd, POLLIN);
            io_uring_sqe_set_data64(sqe, DW_URING_PACK(DW_URING_OP_POLL, aux));
        }
    }

    return 0;
}
#endif

static void dw_select_del_rd_pos(dw_poll_t *p_poll, int i) {
    p_poll->u.select_fds.n_rd_fd--;
    if (i < p_poll->u.select_fds.n_rd_fd) {
        p_poll->u.select_fds.rd_fd[i] =
                p_poll->u.select_fds.rd_fd[p_poll->u.select_fds.n_rd_fd];
        p_poll->u.select_fds.rd_flags[i] =
                p_poll->u.select_fds.rd_flags[p_poll->u.select_fds.n_rd_fd];
        p_poll->u.select_fds.rd_aux[i] =
                p_poll->u.select_fds.rd_aux[p_poll->u.select_fds.n_rd_fd];
    }
}

static void dw_select_del_wr_pos(dw_poll_t *p_poll, int i) {
    p_poll->u.select_fds.n_wr_fd--;
    if (i < p_poll->u.select_fds.n_wr_fd) {
        p_poll->u.select_fds.wr_fd[i] =
                p_poll->u.select_fds.wr_fd[p_poll->u.select_fds.n_wr_fd];
        p_poll->u.select_fds.wr_flags[i] =
                p_poll->u.select_fds.wr_flags[p_poll->u.select_fds.n_wr_fd];
        p_poll->u.select_fds.wr_aux[i] =
                p_poll->u.select_fds.wr_aux[p_poll->u.select_fds.n_wr_fd];
    }
}

static void dw_poll_del_pos(dw_poll_t *p_poll, int i) {
    p_poll->u.poll_fds.n_pollfds--;
    if (i < p_poll->u.poll_fds.n_pollfds) {
        p_poll->u.poll_fds.pollfds[i] = p_poll->u.poll_fds.pollfds[p_poll->u.poll_fds.n_pollfds];
        p_poll->u.poll_fds.flags[i] = p_poll->u.poll_fds.flags[p_poll->u.poll_fds.n_pollfds];
        p_poll->u.poll_fds.aux[i] = p_poll->u.poll_fds.aux[p_poll->u.poll_fds.n_pollfds];
    }
}

static void dw_select_add_rd(dw_poll_t *p_poll, int fd, dw_poll_flags flags, uint64_t aux) {
    p_poll->u.select_fds.rd_fd[p_poll->u.select_fds.n_rd_fd] = fd;
    p_poll->u.select_fds.rd_aux[p_poll->u.select_fds.n_rd_fd] = aux;
    p_poll->u.select_fds.rd_flags[p_poll->u.select_fds.n_rd_fd] = flags;
    p_poll->u.select_fds.n_rd_fd++;
}

static void dw_select_add_wr(dw_poll_t *p_poll, int fd, dw_poll_flags flags, uint64_t aux) {
    p_poll->u.select_fds.wr_fd[p_poll->u.select_fds.n_wr_fd] = fd;
    p_poll->u.select_fds.wr_aux[p_poll->u.select_fds.n_wr_fd] = aux;
    p_poll->u.select_fds.wr_flags[p_poll->u.select_fds.n_wr_fd] = flags;
    p_poll->u.select_fds.n_wr_fd++;
}

int dw_poll_add(dw_poll_t *p_poll, int fd, dw_poll_flags flags, uint64_t aux,
                char *buffer, size_t buffer_len) {
    dw_log("dw_poll_add(): fd=%d, p_poll=%p, flags=%08x, aux=%lu, buf=%p, buf_len=%zu\n",
           fd, p_poll, flags, aux, buffer, buffer_len);
    int rv = 0;

    // record / refresh the per-fd buffer slot ;
    // also used by dw_poll_mod() re-adds (buffer/buffer_len are NULL/0 on those)
    // so leave it untouched
    if (buffer != NULL || buffer_len != 0 || dw_poll_get_fd_buf(p_poll, fd) == NULL) {
        if (p_poll->n_fd_bufs >= MAX_POLLFD) {
            dw_log("Exhausted fd_bufs slots\n");
            return -1;
        }
        dw_poll_fd_buf_t *e = dw_poll_get_fd_buf(p_poll, fd);
        if (!e) {
            e = &p_poll->fd_bufs[p_poll->n_fd_bufs++];
            e->fd = fd;
            e->sendfile_buf = NULL;
            e->sendfile_buf_len = 0;
        }
        e->buffer = buffer;
        e->buffer_len = buffer_len;
        e->write_off = 0;
        e->last_off = 0;
    }

    switch (p_poll->poll_type) {
        case DW_SELECT:
            if ((flags & DW_POLLIN && p_poll->u.select_fds.n_rd_fd == MAX_POLLFD)
                || (flags & DW_POLLOUT && p_poll->u.select_fds.n_wr_fd == MAX_POLLFD)) {
                dw_log("Exhausted number of possible fds in select()\n");
                return -1;
            }
            if (flags & DW_POLLIN)
                dw_select_add_rd(p_poll, fd, flags, aux);
            if (flags & DW_POLLOUT)
                dw_select_add_wr(p_poll, fd, flags, aux);
            break;
        case DW_POLL:
            if (p_poll->u.poll_fds.n_pollfds == MAX_POLLFD) {
                dw_log("Exhausted number of possible fds in poll()\n");
                return -1;
            }
            struct pollfd *pev = &p_poll->u.poll_fds.pollfds[p_poll->u.poll_fds.n_pollfds];
            p_poll->u.poll_fds.flags[p_poll->u.poll_fds.n_pollfds] = flags;
            p_poll->u.poll_fds.aux[p_poll->u.poll_fds.n_pollfds] = aux;
            p_poll->u.poll_fds.n_pollfds++;
            pev->fd = fd;
            pev->events = (flags & DW_POLLIN ? POLLIN : 0)
                          | (flags & DW_POLLOUT ? POLLOUT : 0);
            break;
        case DW_EPOLL: {
            struct epoll_event ev = (struct epoll_event){
                .data.u64 = aux,
                .events = (flags & DW_POLLIN ? EPOLLIN : 0)
                          | (flags & DW_POLLOUT ? EPOLLOUT : 0)
                          | (flags & DW_POLLONESHOT ? EPOLLONESHOT : 0),
            };

            if ((rv = epoll_ctl(p_poll->u.epoll_fds.epollfd, EPOLL_CTL_ADD, fd, &ev)) < 0)
                perror("epoll_ctl() failed: ");
            break;
        }
        case DW_IOURING: {
            #ifdef IOURING_ENABLED

            // POLLIN, register the first read/accept
            // re-arm of happens inside the corresponding dw_io wrapper after each completion
            rv = dw_uring_arm_pollin(p_poll, fd, flags, aux);

            // POLLOUT, at add time is a no-op
            // sends' SQE happens dw_sendto()/dw_sendfile()
            #else
            check(0, "DW_IOURING used without IOURING_ENABLED");
            #endif
            break;
        }
        default:
            check(0, "Wrong dw_poll_type");
    }
    return rv;
}

/* This tolerates on purpose being called after a ONESHOT event, for which
 * poll and select will have deleted the fd in their user-space lists, whilst
 * epoll has it still registered, but with a clear events interest list
 */
int dw_poll_mod(dw_poll_t *p_poll, int fd, dw_poll_flags flags, uint64_t aux) {
    dw_log("dw_poll_mod(): fd=%d, p_poll=%p, flags=%08x, aux=%lu\n", fd, p_poll, flags, aux);
    int rv = 0;

    // drop fd_buf registration when fully removing this fd from the poll set
    if (flags == 0)
        dw_poll_remove_fd_buf(p_poll, fd);

    switch (p_poll->poll_type) {
        case DW_SELECT: {
            int i;
            for (i = 0; i < p_poll->u.select_fds.n_rd_fd; i++)
                if (p_poll->u.select_fds.rd_fd[i] == fd)
                    break;
            if (i < p_poll->u.select_fds.n_rd_fd && !(flags & DW_POLLIN))
                dw_select_del_rd_pos(p_poll, i);
            else if (i == p_poll->u.select_fds.n_rd_fd && (flags & DW_POLLIN))
                dw_select_add_rd(p_poll, fd, flags, aux);

            for (i = 0; i < p_poll->u.select_fds.n_wr_fd; i++)
                if (p_poll->u.select_fds.wr_fd[i] == fd)
                    break;
            if (i < p_poll->u.select_fds.n_wr_fd && !(flags & DW_POLLOUT))
                dw_select_del_wr_pos(p_poll, i);
            else if (i == p_poll->u.select_fds.n_wr_fd && (flags & DW_POLLOUT))
                dw_select_add_wr(p_poll, fd, flags, aux);
            break;
        }
        case DW_POLL: {
            int i;
            for (i = 0; i < p_poll->u.poll_fds.n_pollfds; i++) {
                struct pollfd *pev = &p_poll->u.poll_fds.pollfds[i];
                if (pev->fd == fd) {
                    pev->events = (flags & DW_POLLIN ? POLLIN : 0) | (flags & DW_POLLOUT ? POLLOUT : 0);
                    dw_log("dw_poll_mod(): pev->events=%04x\n", pev->events);
                    if (pev->events == 0)
                        dw_poll_del_pos(p_poll, i);
                    break;
                }
            }
            if (i == p_poll->u.poll_fds.n_pollfds && flags != 0) {
                // preserve existing buffer registration on re-add
                dw_poll_fd_buf_t *e = dw_poll_get_fd_buf(p_poll, fd);
                rv = dw_poll_add(p_poll, fd, flags, aux,
                    e ? e->buffer : NULL,
                    e ? e->buffer_len : 0);
            }
            break;
        }
        case DW_EPOLL: {
            struct epoll_event ev = {
                .data.u64 = aux,
                .events = (flags & DW_POLLIN ? EPOLLIN : 0) | (flags & DW_POLLOUT ? EPOLLOUT : 0),
            };
            if (flags & (DW_POLLIN | DW_POLLOUT))
                rv = epoll_ctl(p_poll->u.epoll_fds.epollfd, EPOLL_CTL_MOD, fd, &ev);
            else
                rv = epoll_ctl(p_poll->u.epoll_fds.epollfd, EPOLL_CTL_DEL, fd, NULL);
            break;
        }
        case DW_IOURING:
            #ifdef IOURING_ENABLED
            if (flags == 0) {
                // cancel any in-flight read SQE for this fd
                struct io_uring_sqe *sqe = io_uring_get_sqe(&p_poll->u.iouring_fds.ring);
                if (sqe) {
                    io_uring_prep_cancel_fd(sqe, fd, 0);
                    // mark so that dw_poll_next can filter it from the main loop
                    io_uring_sqe_set_data64(sqe, DW_URING_PACK(DW_URING_OP_CANCEL, 0));
                }
            }

            // POLLIN/POLLOUT mods are no-ops, see dw_poll_add
            #else
            check(0, "DW_IOURING used without IOURING_ENABLED");
            #endif
            break;
        default:
            check(0, "Wrong dw_poll_type");
    }
    return rv;
}

// return the number of file descriptors expected to iterate with dw_poll_next(),
// or -1 setting errno
int dw_poll_wait(dw_poll_t *p_poll) {
    int rv;
    switch (p_poll->poll_type) {
        case DW_SELECT:
            FD_ZERO(&p_poll->u.select_fds.rd_fds);
            FD_ZERO(&p_poll->u.select_fds.wr_fds);
            FD_ZERO(&p_poll->u.select_fds.ex_fds);
            int max_fd = 0;
            for (int i = 0; i < p_poll->u.select_fds.n_rd_fd; i++) {
                FD_SET(p_poll->u.select_fds.rd_fd[i], &p_poll->u.select_fds.rd_fds);
                if (p_poll->u.select_fds.rd_fd[i] > max_fd)
                    max_fd = p_poll->u.select_fds.rd_fd[i];
            }
            for (int i = 0; i < p_poll->u.select_fds.n_wr_fd; i++) {
                FD_SET(p_poll->u.select_fds.wr_fd[i], &p_poll->u.select_fds.wr_fds);
                if (p_poll->u.select_fds.wr_fd[i] > max_fd)
                    max_fd = p_poll->u.select_fds.wr_fd[i];
            }
            dw_log("select()ing: max_fd=%d\n", max_fd);
            struct timeval null_tout = {.tv_sec = 0, .tv_usec = 0};
            rv = select(max_fd + 1, &p_poll->u.select_fds.rd_fds, &p_poll->u.select_fds.wr_fds, &p_poll->u.select_fds.ex_fds,
                        p_poll->use_spinning ? &null_tout : NULL);
            // make sure we don't wastefully iterate if select() returned 0 fds ready or error
            p_poll->u.select_fds.iter = rv > 0 ? 0 : p_poll->u.select_fds.n_rd_fd + p_poll->u.select_fds.n_wr_fd;
            break;
        case DW_POLL:
            dw_log("poll()ing: n_pollfds=%d\n", p_poll->u.poll_fds.n_pollfds);
            rv = poll(p_poll->u.poll_fds.pollfds, p_poll->u.poll_fds.n_pollfds,
                      p_poll->use_spinning ? 0 : -1);
            // make sure we don't wastefully iterate if poll() returned 0 fds ready or error
            p_poll->u.poll_fds.iter = rv > 0 ? 0 : p_poll->u.poll_fds.n_pollfds;
            break;
        case DW_EPOLL:
            dw_log("epoll_wait()ing: epollfd=%d\n", p_poll->u.epoll_fds.epollfd);
            rv = epoll_wait(p_poll->u.epoll_fds.epollfd, p_poll->u.epoll_fds.events, MAX_POLLFD,
                            p_poll->use_spinning ? 0 : -1);
            p_poll->u.epoll_fds.iter = 0;
            if (rv >= 0)
                p_poll->u.epoll_fds.n_events = rv;
            break;
        case DW_IOURING: {
            #ifdef IOURING_ENABLED
            struct io_uring *r = &p_poll->u.iouring_fds.ring;
            if (p_poll->use_spinning) {
                io_uring_submit(r);
            } else {
                int srv = io_uring_submit_and_wait(r, 1);
                if (srv < 0) {
                    errno = -srv;
                    rv = -1;
                    break;
                }
            }
            p_poll->u.iouring_fds.n_cqes = io_uring_peek_batch_cqe(r, p_poll->u.iouring_fds.cqes, p_poll->u.iouring_fds.cqe_batch_limit);
            p_poll->u.iouring_fds.iter = -1;
            p_poll->u.iouring_fds.consumed_since_submit = 0;
            rv = p_poll->u.iouring_fds.n_cqes;
            #else
            check(0, "DW_IOURING used without IOURING_ENABLED");
            #endif
            break;
        }
        default:
            check(0, "Wrong dw_poll_type");
    }
    return rv;
}

#ifdef IOURING_ENABLED
// returns the CQE most recently emitted by dw_poll_next() or NULL if marked as seen
struct io_uring_cqe *dw_poll_current_cqe(dw_poll_t *p_poll) {
    if (p_poll->poll_type != DW_IOURING)
        return NULL;
    if (p_poll->u.iouring_fds.iter < 0 ||
        p_poll->u.iouring_fds.iter >= (int)p_poll->u.iouring_fds.n_cqes)
        return NULL;

    // returns NULL once dw_poll_cqe_seen() has invalidated the slot
    return p_poll->u.iouring_fds.cqes[p_poll->u.iouring_fds.iter];
}

// wrappers call this after they've read cqe->res to release the slot
void dw_poll_cqe_seen(dw_poll_t *p_poll, struct io_uring_cqe *cqe) {
    if (p_poll->poll_type != DW_IOURING || !cqe)
        return;

    if (p_poll->u.iouring_fds.iter >= 0 &&
        p_poll->u.iouring_fds.cqes[p_poll->u.iouring_fds.iter] == cqe) {
        p_poll->u.iouring_fds.cqes[p_poll->u.iouring_fds.iter] = NULL;
    }

    io_uring_cqe_seen(&p_poll->u.iouring_fds.ring, cqe);
}

struct io_uring_sqe * dw_poll_next_sqe(dw_poll_t *p_poll) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(&p_poll->u.iouring_fds.ring);
    if (!sqe) {
        io_uring_submit(&p_poll->u.iouring_fds.ring);
        sqe = io_uring_get_sqe(&p_poll->u.iouring_fds.ring);
        assert(sqe);
    }
    return sqe;
}
#endif

// returned fd is automatically removed from dw_poll if marked 1SHOT
int dw_poll_next(dw_poll_t *p_poll, dw_poll_flags *flags, uint64_t *aux) {
    switch (p_poll->poll_type) {
        case DW_SELECT:
            while (p_poll->u.select_fds.iter < p_poll->u.select_fds.n_rd_fd && !FD_ISSET(p_poll->u.select_fds.rd_fd[p_poll->u.select_fds.iter],
                                                                                         &p_poll->u.select_fds.rd_fds))
                p_poll->u.select_fds.iter++;
            if (p_poll->u.select_fds.iter < p_poll->u.select_fds.n_rd_fd && FD_ISSET(p_poll->u.select_fds.rd_fd[p_poll->u.select_fds.iter],
                                                                                     &p_poll->u.select_fds.rd_fds)) {
                *aux = p_poll->u.select_fds.rd_aux[p_poll->u.select_fds.iter];
                *flags = DW_POLLIN;

                if (p_poll->u.select_fds.rd_flags[p_poll->u.select_fds.iter] & DW_POLLONESHOT)
                    // item iter replaced with last, so we need to check iter again
                    dw_select_del_rd_pos(p_poll, p_poll->u.select_fds.iter);
                else
                    p_poll->u.select_fds.iter++;
                return 1;
            }
            int n_rd = p_poll->u.select_fds.n_rd_fd;
            while (p_poll->u.select_fds.iter < n_rd + p_poll->u.select_fds.n_wr_fd && !FD_ISSET(
                       p_poll->u.select_fds.wr_fd[p_poll->u.select_fds.iter - n_rd], &p_poll->u.select_fds.wr_fds))
                p_poll->u.select_fds.iter++;
            if (p_poll->u.select_fds.iter < n_rd + p_poll->u.select_fds.n_wr_fd && FD_ISSET(
                    p_poll->u.select_fds.wr_fd[p_poll->u.select_fds.iter - n_rd], &p_poll->u.select_fds.wr_fds)) {
                *aux = p_poll->u.select_fds.wr_aux[p_poll->u.select_fds.iter - n_rd];
                *flags = DW_POLLOUT;
                if (p_poll->u.select_fds.wr_flags[p_poll->u.select_fds.iter - n_rd] & DW_POLLONESHOT)
                    // item iter replaced with last, so we need to check iter again
                    dw_select_del_wr_pos(p_poll, p_poll->u.select_fds.iter - n_rd);
                else
                    p_poll->u.select_fds.iter++;
                return 1;
            }
            break;
        case DW_POLL:
            while (p_poll->u.poll_fds.iter < p_poll->u.poll_fds.n_pollfds && p_poll->u.poll_fds.pollfds[p_poll->u.poll_fds.iter].revents == 0)
                p_poll->u.poll_fds.iter++;
            if (p_poll->u.poll_fds.iter < p_poll->u.poll_fds.n_pollfds && p_poll->u.poll_fds.pollfds[p_poll->u.poll_fds.iter].revents != 0) {
                *flags = 0;
                *flags |= (p_poll->u.poll_fds.pollfds[p_poll->u.poll_fds.iter].revents & POLLIN) ? DW_POLLIN : 0;
                *flags |= (p_poll->u.poll_fds.pollfds[p_poll->u.poll_fds.iter].revents & POLLOUT) ? DW_POLLOUT : 0;
                *flags |= (p_poll->u.poll_fds.pollfds[p_poll->u.poll_fds.iter].revents & POLLERR) ? DW_POLLERR : 0;
                *flags |= (p_poll->u.poll_fds.pollfds[p_poll->u.poll_fds.iter].revents & POLLHUP) ? DW_POLLHUP : 0;
                *aux = p_poll->u.poll_fds.aux[p_poll->u.poll_fds.iter];
                if (p_poll->u.poll_fds.flags[p_poll->u.poll_fds.iter] & DW_POLLONESHOT)
                    // item i replaced with last, so we need to check i again
                    dw_poll_del_pos(p_poll, p_poll->u.poll_fds.iter);
                else
                    p_poll->u.poll_fds.iter++;
                return 1;
            }
            break;
        case DW_EPOLL:
            if (p_poll->u.epoll_fds.iter < p_poll->u.epoll_fds.n_events && p_poll->u.epoll_fds.events[p_poll->u.epoll_fds.iter].events != 0) {
                *flags = 0;
                *flags |= (p_poll->u.epoll_fds.events[p_poll->u.epoll_fds.iter].events & EPOLLIN) ? DW_POLLIN : 0;
                *flags |= (p_poll->u.epoll_fds.events[p_poll->u.epoll_fds.iter].events & EPOLLOUT) ? DW_POLLOUT : 0;
                *flags |= (p_poll->u.epoll_fds.events[p_poll->u.epoll_fds.iter].events & EPOLLERR) ? DW_POLLERR : 0;
                *flags |= (p_poll->u.epoll_fds.events[p_poll->u.epoll_fds.iter].events & EPOLLHUP) ? DW_POLLHUP : 0;
                *aux = p_poll->u.epoll_fds.events[p_poll->u.epoll_fds.iter].data.u64;
                p_poll->u.epoll_fds.iter++;
                return 1;
            }
            break;
        case DW_IOURING: {
            #ifdef IOURING_ENABLED
            // dw_poll_wait sets n_cqes
            while (++p_poll->u.iouring_fds.iter < p_poll->u.iouring_fds.n_cqes) {
                struct io_uring_cqe *cqe = p_poll->u.iouring_fds.cqes[p_poll->u.iouring_fds.iter];
                if (cqe->flags & IORING_CQE_F_MORE) {
                    dw_log("URING: MORE set ignoring, the second one will do the job\n");
                    io_uring_cqe_seen(&p_poll->u.iouring_fds.ring, cqe);
                    p_poll->u.iouring_fds.cqes[p_poll->u.iouring_fds.iter] = NULL;
                    continue;
                }

                struct uring_aux_data*data = (void*)io_uring_cqe_get_data64(cqe);
                dw_uring_op_t         op   = data->op;

                // filter/handle CQE for cancellation operation
                if (op == DW_URING_OP_CANCEL) {
                    io_uring_cqe_seen(&p_poll->u.iouring_fds.ring, cqe);
                    p_poll->u.iouring_fds.cqes[p_poll->u.iouring_fds.iter] = NULL;
                    continue;
                }

                // re-extract aux data
                uint32_t ev_type = 0, ev_id = 0;
                l2i(data->aux, &ev_type, &ev_id);

                // parse CQE status flags
                int32_t res = cqe->res;

                // connection closing filter: handles CQE submitted after a conn_free
                if (res == -ECANCELED || data->out_in_flight) {
                    io_uring_cqe_seen(&p_poll->u.iouring_fds.ring, cqe);
                    p_poll->u.iouring_fds.cqes[p_poll->u.iouring_fds.iter] = NULL;
                    // TODO: actually free
                    continue;
                }

                *aux = data->aux;
                *flags = data->out_requested && !data->out_in_flight ? DW_POLLOUT : 0;

                // handle status errors
                if (res < 0) {
                    if (res == -EPIPE || res == -ECONNRESET)
                        *flags |= DW_POLLHUP;
                    else
                        *flags |= DW_POLLERR;
                }

                // convert from OP to POLL* flags
                switch (op) {
                    case DW_URING_OP_ACCEPT:
                    case DW_URING_OP_RECV:
                    case DW_URING_OP_POLL:
                        *flags |= DW_POLLIN;
                        break;
                    case DW_URING_OP_SEND:
                    case DW_URING_OP_SENDFILE:
                    case DW_URING_OP_SENDFILE_FAILED:
                        *flags |= DW_POLLOUT | DW_POLLOUT_CQE;
                        break;
                    case DW_URING_OP_CANCEL:
                        __builtin_unreachable();
                        break;
                }

                return 1;
            }
            #else
            check(0, "DW_IOURING used without IOURING_ENABLED");
            #endif
            break;
        }
        default:
            check(0, "Wrong dw_poll_type");
    }
    return 0;
}
