TODO
===

## Main

- [ ] avoid resetting prio if it is already set?
- [x] debug with an older client (04/08/2025: `dw_client...`)
- [x] uring+taskset+chrt makes it goes slow (SQPOLL)

- [X] splice+sendv2
- [x] finish replacing get_sqe
- [ ] uring batch limit needs experiments/tuning
- [ ] recvfrom (UDP)

- [x] review all uses of queue.h to account for data_prt freeing (alloca)
- [ ] test_client_opts: `date %s` ha rounding che può farlo fallire a caso
- [~] conns are not freeing nor anything (busy stays at 1)

- [ ] check consumed_since_submit

## Flavour

- [ ] uring multishot
- [~] Malloc use-after-free GCC attribute+warning (asan is good enough)
- [~] dw_client as dw_node (out of scope)
- [ ] ifdef SSL
- [ ] we will need another way to differentiate if we ever need to support disk IO
- [ ] libssl
- [ ] maybe remove all syscalls from uring path



## Results

IORING_RECVSEND_POLL_FIRST adds 2us on average