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

EXCLUDE_PATTERNS=(
    "*proxy*"
    #"*ramp*"
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

SKIP_URING_RE='test_poll_mode\.sh$|test_ssl\.sh$'

for test in "${TESTS[@]}"; do
    if [[ "$test" == *dpdk* ]]; then
        for mode in veth vf; do
            DPDK_MODE=$mode run_test "$test" "$test (dpdk=$mode)"
        done
    else
        run_test "$test" "$test"
        if ! [[ "$test" =~ $SKIP_URING_RE ]]; then
            POLL_MODE=uring run_test "$test" "$test (poll-mode=uring)"
        fi
    fi
done

sleep 1

for d in gcov/*; do
    cp ../src/*.gcno $d
done

gcovr --object-directory ../src --root ../ --gcov-ignore-parse-errors
