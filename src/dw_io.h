#ifndef __DW_IO_H__
#define __DW_IO_H__

#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>

#include "dw_poll.h"

/*
 * Backend-dispatching I/O wrappers.
 *   For select/poll/epoll backends (and for a NULL p_poll, used by dw_client),
 *   these are thin shims over the libc syscalls.
 * These wrappers preserve libc errno semantics:
 *   - return -1 with errno set on failure,
 *   - return byte count / new fd on success.
 */


int dw_accept(dw_poll_t *p_poll, int listen_fd,
              struct sockaddr_in *addr, socklen_t *addrlen);

/*
 * Two part send, for non-uring backends part is ignored and the syscall is called.
 *
 * Under io_uring:
 *   part == 0 (submit): prepare a SQE for and return -1, errno=EAGAIN
 *   part == 1 (complete): consume the CQE
 *
 * aux is used as user data on the SQE so the CQE is routed back to the correct connection.
 * Ignored on non-uring backends and on part == 1.
 */
ssize_t dw_sendto(dw_poll_t *p_poll, int sock, int part, uint64_t aux,
                  const void *buf, size_t len, int flags,
                  const struct sockaddr *dest, socklen_t dest_len);

/*
 * dw_recvfrom writes into the buffer registered with dw_poll_add() for this fd.
 * The buffer's write head (write_off) advances on each successful call.
 * Rewind via dw_poll_set_buffer_offset()
 */
ssize_t dw_recvfrom(dw_poll_t *p_poll, int sock,
                    void **out_buf, int flags,
                    struct sockaddr *from, socklen_t *from_len);

// 0. Allocates and registers a buffer in uring (lazily initialized)
// 1. send SQE for header
// 2. link SQE a read_fixed from the conn->file_fd and conn->file_offset
// 3. link SQE for a send_zc of that buffer
ssize_t dw_sendfile(dw_poll_t *p_poll, int sock, int part, uint64_t aux,
                    const void *hdr, size_t hdr_len,
                    int fd_in, off_t off_in, size_t len);

#endif /* __DW_IO_H__ */
