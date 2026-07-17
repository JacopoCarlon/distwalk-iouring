#!/bin/bash

COL_RED='\033[0;32m'
COL_GRN='\e[0;31m'
COL_YLW='\e[0;33m'
COL_DEF='\e[m'

rm -rf *.gcda *.gcov log_tests.txt gcov/ ../src/*.gcda ../src/gcov/
trap 'exit' SIGINT SIGTERM

#cp ../src/dw_client ../src/dw_client_debug ../src/dw_client_tsan ../src/dw_node ../src/dw_node_debug ../src/dw_node_tsan .

TESTS=
if [ $# -gt 0 ]; then
    TESTS=( $@ )
else
    TESTS=( $(find ../src/ -name 'test_*' -executable | grep -v '~$') $(ls test_*.sh) )
fi


echo "Before filtering:"
printf '  - %s\n' "${TESTS[@]}"
echo "done listing before testing"

EXCLUDE_PATTERNS=(
    "*ramp*"            ## -> works : skip ramp because it is slow. It works.
    "*proxy*"           ## BROKEN __ skip because it never works.
    "*est_skip.*"       ## BROKEN + MEGA CRASH ... test_skip is broken.
    #   "*est_forward.sh"       ## BROKEN : forward.sh 
    #   "*est_forward_self*"            ## -> works, but keep an eye
    #   "*est_forward_skip*"            ## -> works, but keep an eye
    #   "*est_forward_timeout*"         ## -> works, but keep an eye
    #   "*est_multi_forward*"           ## -> works, but keep an eye
    "*est_ssl*"                     ## -> works, but keep an eye 
    "*est_accept_mode.s*"           ## -> works, possibly doublecheck
    "*est_accept_mode_parent.s*"    ## -> works, possibly doublecheck     
    "*est_conn_drop*"               ## -> works, possibly doublecheck
    "*est_poll_mode*"               ## -> works, possibly doublecheck
    "*est_retry*"                   ## -> works, possibly doublecheck
    "*est_simple*"                  ## -> works, possibly doublecheck
    "*est_client_opts*"             ## -> works      
    "*est_client_out*"              ## -> works
    "*est_compute*"                 ## -> works      
    "*est_connect*"                 ## -> works
    "*est_distrib*"                 ## -> works
    "*est_loadstor*"                ## -> works : loadstore and odirect    
    "*est_node*"                    ## -> works
    "*est_sched*"                   ## -> works
    "*est_script*"                  ## -> works
    "*est_stats*"                   ## -> works
)

is_excluded() {
    local name="$1"
    for pat in "${EXCLUDE_PATTERNS[@]}"; do
        case "$name" in
            $pat) return 0 ;;   # 0 = true --> excluded
        esac
    done
    return 1
}

if [[ ${#EXCLUDE_PATTERNS[@]} -gt 0 ]]; then
    filtered=()
    for t in "${TESTS[@]}"; do
        if is_excluded "$t"; then
            echo "  [EXCLUDED] $t" >&2
        else
            filtered+=("$t")
        fi
    done
    TESTS=("${filtered[@]}")
fi


run_test() {
    local test=$1
    local label=$2
    echo -n "TEST $label: "
    echo -e "\n\nTEST $label:\n" >> log_tests.txt
    bash -c ./$test >> log_tests.txt 2>&1
    rc=$?
    if [ $rc -eq 0 ]; then
        echo -e "${COL_RED}SUCCESS${COL_DEF}"
    elif [ $rc -eq 77 ]; then
        echo -e "${COL_YLW}SKIPPED${COL_DEF} (non-root or missing USE_DPDK)"
    else
        echo -e "${COL_GRN}ERROR${COL_DEF}"
    fi
}


RUN_ONCE='test_poll_mode\.sh$'          ## test_poll_mode.sh already tests all poll modes, no need to iterate over them.
SKIP_EPOLL_RE='test_poll_mode\.sh$'
SKIP_POLL_RE='test_poll_mode\.sh$'
SKIP_SELECT_RE='test_poll_mode\.sh$'

for test in "${TESTS[@]}"; do
    if [[ "$test" == *dpdk* ]]; then
        for mode in veth vf; do
            DPDK_MODE=$mode run_test "$test" "$test (dpdk=$mode)"
        done
        continue
    fi
    
    if [[ "$test" =~ $RUN_ONCE ]]; then
        run_test "$test" "$test (covers all modes)"
        continue 
    fi

    if ! [[ "$test" =~ $SKIP_EPOLL_RE ]]; then
        POLL_MODE=epoll run_test "$test" "$test (poll-mode=epoll)"
    fi

    if ! [[ "$test" =~ $SKIP_POLL_RE ]]; then
        POLL_MODE=poll run_test "$test" "$test (poll-mode=poll)"
    fi

    if ! [[ "$test" =~ $SKIP_SELECT_RE ]]; then
        POLL_MODE=select run_test "$test" "$test (poll-mode=select)"
    fi
done

## TODO: need warning to say that this needs <sudo apt install gcovr>
for d in gcov/*; do
    cp ../src/*.gcno $d
done

gcovr --object-directory ../src --root ../ --gcov-ignore-parse-errors

