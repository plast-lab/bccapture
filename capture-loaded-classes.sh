#!/bin/bash

# Generates the JARs for one or more dacapo-bach benchmarks.

AGENT_NAME=libBytecodeCapture
MANIFEST=dacapo-bach/tradebeans-skeleton/META-INF/MANIFEST.MF

function generateJar {
    echo Generating JAR with loaded classes for $1
    rm -rf out
    java -agentpath:./${AGENT_NAME}.so -jar dacapo-bach/dacapo-9.12-bach.jar $1 |& tee loaded-$1.txt
    # delete cglib code that crashes Soot
    find out -name "*CGLIB\$\$*" -exec rm {} \;
    # make jar_$1
    jar cfm $1-loaded-classes.jar ${MANIFEST} -C out .
}

# dacap-bach choices:
# avrora batik eclipse fop h2 jython luindex lusearch pmd sunflow tomcat tradebeans tradesoap xalan

if [ "$1" == "" ]
then
    for b in avrora batik eclipse fop h2 jython luindex lusearch pmd sunflow tomcat tradebeans tradesoap xalan
    do
	generateJar $b
    done
else
    generateJar $1
fi
