#!/bin/bash

# Generates the JARs for one or more dacapo-bach benchmarks.

AGENT_NAME=libBytecodeCapture
MANIFEST=dacapo-bach/tradebeans-skeleton/META-INF/MANIFEST.MF

function generateJar {
    rm -rf out
    make JDK_INCLUDE=/usr/java/jdk1.7.0_75/include all
    java -agentpath:./${AGENT_NAME}.so -jar dacapo-bach/dacapo-9.12-bach.jar $1 |& tee loaded-$1.txt
    find out -name "*CGLIB\$\$*" -exec rm {} \;
    # make jar_$1
    jar cfm $1-loaded-classes.jar ${MANIFEST} -C out .
}

# dacap-bach choices:
# avrora batik eclipse fop h2 jython luindex lusearch pmd sunflow tomcat tradebeans tradesoap xalan

generateJar avrora
# generateJar tradebeans
