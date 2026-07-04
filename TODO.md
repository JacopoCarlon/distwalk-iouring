# TODO - updated 2026/07/04

---
---
---

## Master Problems we should/could care about (each of thesethese should be PR by themselves)
- [ ] avoid resetting prio if it is already set

- [ ] uring/ssl/dpdk (and all other ifdefs)  
    should become like https://www.kernel.org/doc/html/latest/process/coding-style.html#conditional-compilation
    - [x] ifdef URING

    - [ ] standardixe ifdef
        - [~] ifdef DPDK (could use a double-check) and its includes .h
        - [ ] ifdef SSL

- [x] (SOLVED, to PR) test_client_opts: date %s ha rounding che può farlo fallire a caso

- [ ] all_tests - involving fixes: 
    - [x] fix run-tests to be able to skip tests, 
    - [x] properly name all test-produced tmp files for easier logging/debugging
    - [x] kill-all as executable for comodity 
    - [ ] files in /test/ should be callable from outside that folder  
        (i.e. wrap common.sh properly)

- [ ] all tests- standardize:
  - [ ] wrap tests for SSL and uring (e.g. on kernel version) like they are already for DPDK  
        (i.e. SKIPPED because not compiled/supported/..)

- [x] (PR under review:) Debug all uses of queue.h to account for data_prt freeing (vs allocation)

---
---
---


## URING Problems

- [ ] fare che tutti i test fanno tutti i mode: 
    - [ ] BUG: test_forward.sh (poll-mode=select): ERROR
    - [ ] BUG: test_forward_timeout.sh (poll-mode=poll): ERROR

- [ ] BUG: client > 100 crasha. 

- [ ] need DOUBLE-CHECK : recvfrom (UDP) (passes tests but is sus)

- [x] double-check logic around <consumed_since_submit>
- [x] conns are not freeing nor anything (busy stays at 1)


---
---
---


## Flavour URING
- [ ] maybe remove all syscalls from uring path 

- [ ] we will need another way to differentiate if we ever need to support disk IO
- [ ] uring multishot



---
---
---


## Testing/PLOTTING on URING
- [ ] uring+taskset+chrt makes it goes slow (SQPOLL)  
    -> from master problem on priority setting
- [ ] uring batch limit needs experiments/tuning



---
---
---


## Other Flavouring (possibly out of scope)
- [ ] libssl
- [ ] Malloc use-after-free GCC attribute+warning (asan is good enough)
- [ ] dw_client as dw_node (out of scope)



---
---
---


## SOLVED

- [x] debug with an older client (04/08/2025: dw_client...)
- [X] splice+sendv2
- [x] finish replacing get_sqe


