#!/bin/sh

# "set -x" expands variables and prints a little + sign before the line
set -e

VALIDATOR=../../src/utils/ncmpivalid/ncmpivalid

for j in 0 1 ; do { \
    export PNETCDF_SAFE_MODE=$$j ; \
    for i in ${TESTPROGRAMS}; do ( \
        ${TESTSEQRUN} ./$i            ${TESTOUTDIR}/testfile.nc ; \
        ${TESTSEQRUN} ${VALIDATOR} -q ${TESTOUTDIR}/testfile.nc ; \
) ; done ; } ; done


${TESTSEQRUN} ./mcoll_perf ${TESTOUTDIR}/testfile
# seq is not available on FreeBSD otherwise we can use: for j in `seq 0 9`
for j in 0 1 2 3 4 5 6 7 8 9 ; do { \
    ${TESTSEQRUN} ${VALIDATOR} -q ${TESTOUTDIR}/testfile.2.4.$j.nc ; \
} ; done
