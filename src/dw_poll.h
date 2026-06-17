#ifndef __DW_POLL_H__
#define __DW_POLL_H__

#include <stddef.h>
#include <stdint.h>
#include <sys/select.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <poll.h>
#include <stdbool.h>

#ifdef IOURING_ENABLED
#include <liburing.h>
#endif

typedef enum { DW_SELECT, DW_POLL, DW_EPOLL, DW_IOURING } dw_poll_type_t;

// these flags are OR-ed both in input and output to dw_poll_*()
typedef enum {
    DW_POLLIN = 1u << 0,
    DW_POLLOUT = 1u << 2,
    DW_POLLERR = 1u << 3,
    DW_POLLHUP = 1u << 4,
    DW_POLLOUT_CQE = 1u << 28,
    DW_ACCEPT = 1u << 29,
    DW_POLLONESHOT = 1u << 30
} dw_poll_flags;

#define MAX_POLLFD 8192
#define MAX_POLL_EVENTS 16

typedef struct {
    dw_poll_type_t poll_type;
    int use_spinning; // 1 = busy-poll, 0 blocking wait

    union {
        struct {
            int rd_fd[MAX_POLLFD];
            int wr_fd[MAX_POLLFD];
            uint64_t rd_aux[MAX_POLLFD];
            uint64_t wr_aux[MAX_POLLFD];
            dw_poll_flags rd_flags[MAX_POLLFD];
            dw_poll_flags wr_flags[MAX_POLLFD];
            int n_rd_fd;
            int n_wr_fd;
            fd_set rd_fds, wr_fds, ex_fds;
            int iter; // from 0 to n_rd_fd + n_wr_fd - 1
        } select_fds;

        struct {
            struct pollfd pollfds[MAX_POLLFD];
            uint64_t aux[MAX_POLLFD];
            dw_poll_flags flags[MAX_POLLFD];
            int n_pollfds;
            int iter;
        } poll_fds;

        struct {
            struct epoll_event events[MAX_POLLFD];
            int epollfd;
            int n_events;
            int iter;
        } epoll_fds;

        #ifdef IOURING_ENABLED
        struct {
            struct io_uring ring;
            struct io_uring_cqe *cqes[MAX_POLL_EVENTS];
            int n_cqes;
            int iter; // points to current slot, -1 = none
            int cqe_batch_limit; // re-submit SQEs after this many CQEs
            int consumed_since_submit;
            bool sendfile_bufs_registered;
        } iouring_fds;
        #endif
    } u;
} dw_poll_t;

typedef enum {
    DW_URING_OP_POLL = 0,
    DW_URING_OP_RECV = 1,
    DW_URING_OP_ACCEPT = 2,
    DW_URING_OP_SEND = 3,
    DW_URING_OP_CANCEL = 4, // internal: fd-removal cancel; never a dispatcher event
    DW_URING_OP_SENDFILE = 5,
    DW_URING_OP_SENDFILE_FAILED = 6,
    DW_URING_OP_POLL_CONNECTING = 7,
} dw_uring_op_t;

#define DW_URING_AUX_MASK   ((1ULL << 60) - 1)
#define DW_URING_PACK(op, aux)  ((((uint64_t)(op)) << 60) | ((uint64_t)(aux) & DW_URING_AUX_MASK))
#define DW_URING_UNPACK_OP(ud)  ((dw_uring_op_t)((ud) >> 60))
#define DW_URING_UNPACK_AUX(ud) ((ud) & DW_URING_AUX_MASK)

// initialize the list of monitored fds
int dw_poll_init(dw_poll_t *p_poll, dw_poll_type_t type, int use_spinning);

// add fd to the list of monitored fds, with associated custom data aux.
// TODO: desc
int dw_poll_add(dw_poll_t *p_poll, int fd, dw_poll_flags flags, uint64_t aux, int conn_id);

#ifdef IOURING_ENABLED
// Last CQE emitted by dw_poll_next() (NOT yet marked seen). Used by dw_io
// wrappers to read cqe->res then call dw_poll_cqe_seen().
struct io_uring_cqe *dw_poll_current_cqe(dw_poll_t *p_poll);

// Release a CQE slot. Also drains the SQ if more than cqe_batch_limit CQEs
// have been consumed since the last submit (preserves uring batching while
// keeping the SQ from starving).
void dw_poll_cqe_seen(dw_poll_t *p_poll, struct io_uring_cqe *cqe);

// Get a pointer to the next free SQE
struct io_uring_sqe * dw_poll_next_sqe(dw_poll_t *p_poll);

#endif

// modify fd in the list of monitored fds
// use rd == wr == 0 to delete fd from the list of monitored fds
int dw_poll_mod(dw_poll_t *p_poll, int fd, dw_poll_flags flags, uint64_t aux);

// remove fd from the list of monitored fds
static inline int dw_poll_del(dw_poll_t *p_poll, int fd) {
    return dw_poll_mod(p_poll, fd, 0, 0);
}

// block waiting for any fd to have an event
int dw_poll_wait(dw_poll_t *p_poll);

// after a successful return of dw_poll_wait(), return the next fd,
// its associated events in *p_rd/*p_wr, and custom data in *p_aux,
// or return 0 if there are no more fds
int dw_poll_next(dw_poll_t *p_poll, dw_poll_flags *p_flags, uint64_t *p_aux);


#endif /* __DW_POLL_H__ */
