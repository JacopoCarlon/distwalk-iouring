TODO
===

## Main

- [ ] splice+sendv2
- [ ] finish replacing get_sqe
- [ ] uring batch limit needs experiments/tuning
- [ ] recvfrom

- [ ] review all uses of queue.h to account for data_prt freeing
- [ ] test_client_opts: `date %s` ha rounding che può farlo fallire a caso 
- [ ] conns are not freeing nor anything (busy stays at 1)

- [ ] check consumed_since_submit

## Flavour

- [ ] uring multishot
- [ ] Malloc use-after-free GCC attribute+warning
- [ ] dw_client as dw_node
- [ ] ifdef SSL
- [ ] we will need another way to differentiate if we ever need to support disk IO
- [ ] libssl
- [ ] maybe remove all syscalls from uring path