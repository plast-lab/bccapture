A bytecode capture agent for the JVM. Based on:

  http://cr.openjdk.java.net/~iklam/jdk9/8155239_simple_classfileloadhook.v01/raw_files/new/test/testlibrary/libSimpleClassFileLoadHook.c

Requires a JVMTI-capable JVM and the "mkdir" command from GNU coreutils.

To compile it:

1. Edit the Makefile and configure the ```JDK_INCLUDE``` path to point to the
directory containing jvmti.h. To run the tests based on
doop-benchmarks, also set the ```DOOP_BENCHMARKS``` path.

2. ```make c_agent```

To run the tests:

```make tests```

Or, individually:

```
make test_run_example
make test_run_proxy_example
make test_run_cafe
```

Tests "test_run_proxy_example" and "test_run_cafe" need a correct
```DOOP_BENCHMARKS``` path.