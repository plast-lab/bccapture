#!/bin/bash

# Fuses a dacapo-bach JAR with the extra classes that are observed
# when it runs. The result JAR contains the original JAR classes plus
# all classes only existing in the second JAR.
#
# Usage: ./fuse-jars.sh original.jar captured.jar result.jar
#
#   original.jar : original JAR from doop-benchmarks/dacapo-bach
#   captured.jar : result of running the agent recording loaded
#                  classes.
#   result.jar   : the name of the JAR to generate.

RESULT_JAR=$3
MANIFEST=dacapo-bach/tradebeans-skeleton/META-INF/MANIFEST.MF

if [ \( "$1" == "" \) -o \( "$2" == "" \) -o \( "$3" == "" \)  ]
then
    echo Usage: ./fuse-jars.sh original.jar captured.jar result.jar
else
    CWD=`pwd`
    OUT_DIR=`mktemp -d`
    echo Using ${OUT_DIR}...
    ORIGINAL_JAR=`realpath $1`
    CAPTURED_JAR=`realpath $2`
    cd ${OUT_DIR}
    jar xf ${CAPTURED_JAR}
    jar xf ${ORIGINAL_JAR}
    cd ${CWD}
    jar cfm ${RESULT_JAR} ${MANIFEST} -C ${OUT_DIR} .
    rm -rf ${OUT_DIR}

    echo 'Statistics (# of classes):'
    echo -n 'Original: '
    jar tf $1 | grep -F '.class' | wc -l
    echo -n 'Captured: '
    jar tf $2 | grep -F '.class' | wc -l
    echo -n 'Fused: '
    jar tf $3 | grep -F '.class' | wc -l
    
fi
