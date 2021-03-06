AGENT_NAME=libBytecodeCapture
MANIFEST=dacapo-bach/tradebeans-skeleton/META-INF/MANIFEST.MF
JDK_INCLUDE=/usr/lib/jvm/java-1.8.0-openjdk/include
DOOP_BENCHMARKS?=${HOME}/doop-benchmarks
GDB=gdb -tui -args
ANDROID_NDK_TOOLCHAIN?=${HOME}/Android/mytooldir-21/android-ndk-21/bin
ANDROID_JVMTI_INCLUDE?=${HOME}/Downloads/android/Android-sources-7.1/art/runtime/openjdkjvmti

all: agent

# c_agent2: ClassLogger.c
# 	gcc -g -shared -fPIC -c -o ClassLogger.o -I $(JDK_INCLUDE) -I $(JDK_INCLUDE)/linux ClassLogger.c
# 	gcc -g -shared -fPIC -o libClassLogger.so ClassLogger.o
# test_run_example_classlogger_c_agent2: compile_example
# 	java -agentpath:~/workspace/jvmti/libClassLogger.so Main

agent: $(AGENT_NAME).cpp
	g++ -g -shared -fPIC -Wall -o $(AGENT_NAME).so -I $(JDK_INCLUDE) -I $(JDK_INCLUDE)/linux -lpthread $(AGENT_NAME).cpp

agent_android: $(AGENT_NAME).cpp
	$(ANDROID_NDK_TOOLCHAIN)/arm-linux-androideabi-g++ -g -shared -fPIC -Wall -o $(AGENT_NAME).so -I $(ANDROID_JVMTI_INCLUDE) $(AGENT_NAME).cpp

clean:
	rm -f ClassLogger.o libBytecodeCapture.so libClassLogger.o libClassLogger.so Main.class some/package1/A.class

# == Tests ==

tests: test_run_example test_run_proxy_example test_run_cafe

# Simple bundled example, contains packages (tests creation of class
# files in subdirectories).
compile_example: Main.java
	javac Main.java some/package1/A.java
test_run_example: compile_example
	java -agentpath:./$(AGENT_NAME).so Main

# Simple proxy example from Doop benchmarks, to test proxy creation.
test_run_proxy_example:
	java -agentpath:./$(AGENT_NAME).so -jar $(DOOP_BENCHMARKS)/proxies/proxy-example-code/Main.jar Main

# Another Doop benchmark with proxies and more classes (including GUI
# elements).
test_run_cafe:
	java -agentpath:./$(AGENT_NAME).so -cp $(DOOP_BENCHMARKS)/proxies/cafebahn/libs/commons-codec-1.3.jar:$(DOOP_BENCHMARKS)/proxies/cafebahn/libs/commons-httpclient-3.0.1.jar:$(DOOP_BENCHMARKS)/proxies/cafebahn/libs/commons-logging-1.1.1.jar:$(DOOP_BENCHMARKS)/proxies/cafebahn/libs/commons-logging-1.1.jar:$(DOOP_BENCHMARKS)/proxies/cafebahn/libs/log4j-1.2.14.jar:$(DOOP_BENCHMARKS)/proxies/cafebahn/libs/org.eclipse.mylyn.wikitext.core_1.6.0.I20120312-2356.jar:$(DOOP_BENCHMARKS)/proxies/cafebahn/libs/org.eclipse.mylyn.wikitext.mediawiki.core_1.6.0.I20120312-2356.jar:$(DOOP_BENCHMARKS)/proxies/cafebahn/libs/RXTXcomm.jar:$(DOOP_BENCHMARKS)/proxies/cafebahn/libs/ws-commons-util-1.0.1.jar:$(DOOP_BENCHMARKS)/proxies/cafebahn/libs/xmlrpc-client-3.0.jar:$(DOOP_BENCHMARKS)/proxies/cafebahn/libs/xmlrpc-common-3.0.jar:$(DOOP_BENCHMARKS)/proxies/cafebahn/libs/xmlrpc-server-3.0.jar:$(DOOP_BENCHMARKS)/proxies/cafebahn/rgzm.jar tochterUhr.gui.Digitaluhr

test_run_tradebeans:
	java -agentpath:./$(AGENT_NAME).so -jar dacapo-bach/dacapo-9.12-bach.jar tradebeans

test_run_h2:
	java -agentpath:./$(AGENT_NAME).so -jar dacapo-bach/dacapo-9.12-bach.jar h2

test_run_sunflow:
	java -agentpath:./$(AGENT_NAME).so -jar dacapo-bach/dacapo-9.12-bach.jar sunflow

test_run_xalan:
	java -agentpath:./$(AGENT_NAME).so -jar dacapo-bach/dacapo-9.12-bach.jar xalan

test_run_avrora:
	java -agentpath:./$(AGENT_NAME).so -jar dacapo-bach/dacapo-9.12-bach.jar avrora

test_run_jython:
	java -agentpath:./$(AGENT_NAME).so -jar dacapo-bach/dacapo-9.12-bach.jar jython

test_run_eclipse:
	java -agentpath:./$(AGENT_NAME).so -jar dacapo-bach/dacapo-9.12-bach.jar eclipse

hprof_tradebeans:
	java -agentlib:hprof=heap=dump,format=b,depth=8 -jar dacapo-bach/dacapo-9.12-bach.jar tradebeans

test_run_tradesoap:
	java -agentpath:./$(AGENT_NAME).so -jar dacapo-bach/dacapo-9.12-bach.jar tradesoap

jar_tradebeans:
	cp -Rf dacapo-bach/tradebeans-skeleton/* out/
	jar cfm tradebeans-loaded-classes.jar ${MANIFEST} -C out .

jar_tradesoap:
	cp -Rf dacapo-bach/tradebeans-skeleton/* out/
	jar cfm tradesoap-loaded-classes.jar ${MANIFEST} -C out .

jar_avrora:
	jar cfm avrora-loaded-classes.jar ${MANIFEST} -C out .

doc:
	doxygen Doxyfile
