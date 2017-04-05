/*
 * Copyright (c) 2016, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <alloca.h>
#include <pthread.h>

#include <jvmti.h>

static jvmtiEnv *jvmti = NULL;
static jvmtiEventCallbacks callbacks;
static jvmtiCapabilities caps;
static int bytecodes[256];

static pthread_mutex_t stats_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t print_lock = PTHREAD_MUTEX_INITIALIZER;
static int defined_sum;
static int defined_by_defineClass;
static int defined_by_defineAnonymousClass;
static int defined_by_unknown;
static int defined_missing;
static int defined_but_ignored;

// Flags to control output in standard output (slow, must be
// serialized) or files (async).
const int USE_STDOUT = 0;
const int USE_FILE = 1;

// From http://stackoverflow.com/questions/4770985/how-to-check-if-a-string-starts-with-another-string-in-c
int starts_with(const char *pre, const char *str)
{
  size_t lenpre = strlen(pre), lenstr = strlen(str);
  return lenstr < lenpre ? 0 : strncmp(pre, str, lenpre) == 0;
}

// Given a directory name, this function calls 'mkdir -' to create it,
// including all its parents.
void make_dirs(const char* out_dir) {
  // '11' is some extra space for "mkdir -p ".
  int mkdir_cmd_len = strlen(out_dir) + 13;
  char* mkdir_cmd = alloca(mkdir_cmd_len);
  int r = snprintf(mkdir_cmd, mkdir_cmd_len, "mkdir -p '%s'", out_dir);
  if (r >= mkdir_cmd_len) {
    printf("Internal error: subdirectory name too long.\n");
    exit(-1);
  }
  system(mkdir_cmd);
}

// Writes a bytecode data stream to a file. Takes the fully-qualifed
// name of the class (e.g. 'package1/package2/C'), the base output
// directory (e.g. 'out'), the full output dir for the target class
// (e.g. 'out/package1/package2'), the length of the class data, and
// the class data byte array.
void writeClass(const char* name, const char* out_base_dir,
                jint class_data_len, const unsigned char* class_data) {
  
  size_t class_file_name_len = strlen(out_base_dir) + 1 + strlen(name) + 7;
  char* class_file_name = alloca(class_file_name_len);
  int r = snprintf(class_file_name, class_file_name_len, "%s/%s.class", out_base_dir, name);
  if (r >= class_file_name_len) {
    printf("Internal error: malformed class file name, wrote %d bytes (out of %ld)\n", r, class_file_name_len);
    exit(-1);
  } else {
    /* // Replace '$' with '_' (e.g. generated proxy classes). */
    /* for (int i = 0; i < strlen(class_file_name); i++) */
    /*   if (class_file_name[i] == '$') */
    /* 	class_file_name[i] = '_'; */
    printf("* Writing %s (%d bytes)...\n", class_file_name, class_data_len);
    FILE* class_file = fopen(class_file_name, "a");
    fwrite(class_data, class_data_len, 1, class_file);
    fclose(class_file);
  }
}

void printClassLoaderInfo(JNIEnv *env, const jobject loader, FILE* context_stream) {
  if (loader == NULL)
    fprintf(context_stream, "[Null classloader (bootstrap?)]\n");
  else {
    // Show information about the class loader.
    jclass loader_class = (*env)->GetObjectClass(env, loader);
    if (loader_class == NULL)
      fprintf(context_stream, "[Error retrieving classloader (#1).]\n");
    else {
      char* loader_sig;
      jvmtiError err = (*jvmti)->GetClassSignature(jvmti, loader_class, &loader_sig, NULL);
      if ((err == JVMTI_ERROR_NONE) && (loader_sig != NULL))
	fprintf(context_stream, "[classloader class: %s]\n", loader_sig);
      else
	fprintf(context_stream, "[Error retrieving classloader (#2).]\n");
    }
  }
  fflush(context_stream);
}

void print_bc(FILE* stream, const unsigned char c) {
  switch (c) {
  case 18: fprintf(stream, "ldc"); break;
  case 19: fprintf(stream, "ldc_w"); break;
  case 178: fprintf(stream, "getstatic"); break;
  case 179: fprintf(stream, "putstatic"); break;
  case 182: fprintf(stream, "invokevirtual"); break;
  case 183: fprintf(stream, "invokespecial"); break;
  case 184: fprintf(stream, "invokestatic"); break;
  case 185: fprintf(stream, "invokeinterface"); break;
  case 186: fprintf(stream, "invokedynamic"); break;
  case 187: fprintf(stream, "new"); break;
  case 189: fprintf(stream, "anewarray"); break;
  case 191: fprintf(stream, "athrow"); break;
  case 193: fprintf(stream, "instanceof"); break;
  case 197: fprintf(stream, "multianewarray"); break;
  default:
    fprintf(stream, "bytecode-%d", c);
  }
}

void count_bytecode_location(jlocation location, jmethodID method_id,
			     FILE* context_stream) {
  jint bytecode_count_ptr;
  unsigned char* bytecodes_ptr;
  jvmtiError bc_err = (*jvmti)->GetBytecodes(jvmti, method_id, &bytecode_count_ptr, &bytecodes_ptr);
  if (bc_err == JVMTI_ERROR_NONE) {
    char bc = bytecodes_ptr[location];

    // No lock here, caller holds the lock.
    // pthread_mutex_lock(&stats_lock);
    bytecodes[(unsigned char)bc]++;
    // pthread_mutex_unlock(&stats_lock);

    fprintf(context_stream, "[bc:"); print_bc(context_stream, bc); fprintf(context_stream, "]");
  }
  else
    fprintf(context_stream, "(error reading bytecode)");
}

void printLocation(jlocation location, jmethodID method_id, int* read_bytecode,
                   FILE* context_stream) {
  jvmtiJlocationFormat locFormat;
  if (location == -1) {
    fprintf(context_stream, "(native method) "); return;
  }
  jvmtiError err1 = (*jvmti)->GetJLocationFormat(jvmti, &locFormat);
  if (err1 != JVMTI_ERROR_NONE) {
    fprintf(context_stream, "(error reading location) "); return;
  }
  else if (locFormat != JVMTI_JLOCATION_JVMBCI) {
    fprintf(context_stream, "(unsupported location type) "); return;
  }
  else {
    fprintf(context_stream, "(bytecode @ position %ld) ", location);
    if (*read_bytecode) {
      count_bytecode_location(location, method_id, context_stream);
      *read_bytecode = 0;
    }

    jint entry_count;
    jvmtiLineNumberEntry* table;
    jvmtiError lines_err = (*jvmti)->GetLineNumberTable(jvmti, method_id, &entry_count, &table);
    if (lines_err == JVMTI_ERROR_NONE) {
      int before = 0, found = 0;
      for (int j = 0; j < entry_count; j++) {
	jlocation loc = table[j].start_location;
	if (loc < location)
	  before = 1;
	else if ((loc >= location) && before) {
	  // This check needs one more instruction after the
	  // one we need. This should always be the case, as
	  // the last instruction is always a non-invoke
	  // (e.g. areturn).
	  fprintf(context_stream, "(candidate line number: %d) ", table[j-1].line_number);
	  found = 1;
	  break;
	}
      }
      if (!found)
	fprintf(context_stream, "(could not determine source location) ");
    }
    else
      fprintf(context_stream, "(source location: error %d) ", lines_err);
  }
}

FILE* choose_stdout_or_file(const char* class_name, const char* out_base_dir,
			    const char* out_dir, const int use_file) {
  if (use_file == USE_STDOUT)
    return stdout;
  else {
    size_t info_file_name_len = strlen(out_dir) + 1 + strlen(class_name) + 7;
    char* info_file_name = alloca(info_file_name_len);
    int r = snprintf(info_file_name, info_file_name_len, "%s/%s.info", out_base_dir, class_name);
    if (r >= info_file_name_len) {
      fprintf(stderr, "Internal error: malformed info file name, wrote %d bytes (out of %ld)\n", r, info_file_name_len);
      exit(-1);
    }
    FILE* context_stream = fopen(info_file_name, "a");
    if (context_stream == NULL) {
      fprintf(stderr, "Internal error: cannot open info file %s\n", info_file_name);
      exit(-1);
    }
    return context_stream;
  }
}

// Reads the stack and finds the innermost method.
void writeExecContext(JNIEnv *env, const char* class_name,
                      const jobject loader, const char* out_base_dir,
                      const char* out_dir, const int use_file) {
  const jint max_frame_count = 47;
  jvmtiFrameInfo* frames = (jvmtiFrameInfo*)calloc(max_frame_count, sizeof(jvmtiFrameInfo));
  jint count;
  jthread current_thread = NULL;
  FILE* context_stream = choose_stdout_or_file(class_name, out_base_dir, out_dir, use_file);

  pthread_mutex_lock(&stats_lock);

  int dc = defined_by_defineClass;
  int dac = defined_by_defineAnonymousClass;
  int du = defined_by_unknown;
  int dm = defined_missing;

  jvmtiError err = (*jvmti)->GetStackTrace(jvmti, current_thread, 0,
					   max_frame_count, frames, &count);
  if (err != JVMTI_ERROR_NONE) {
    fprintf(context_stream, "[error reading stack trace]");
    fflush(context_stream);
    defined_by_unknown++;
  }
  else {
    // Flag to control bytecode reading.
    int read_bytecode = 0;
    for (int i = 0; i < count; i++) {
      char *method_name;
      jmethodID method_id = frames[i].method;
      jlocation location = frames[i].location;
      char* method_sig;
      err = (*jvmti)->GetMethodName(jvmti, method_id,
                                    &method_name, NULL, &method_sig);
      if (err != JVMTI_ERROR_NONE) {
        defined_by_unknown++;
      } else {
        fprintf(context_stream, "{ Frame %d: ", i);
        // fprintf(context_stream, "Class %s loaded while executing method: %s\n", class_name, method_name);
        fprintf(context_stream, "* In method: %s (signature: %s) ", method_name, method_sig == NULL? "no signature" : method_sig);
        // Check topmost method to see if this class is a truly
        // dynamically generated/loaded class. If it's not one of the
        // known class generators/loaders, it must be due to lazy
        // loading; in that case, set read_bytecode to check this
        // frame's bytecode call site and record the opcode there in
        // the stats.
        if (i == 0) {
          if (method_sig != NULL) {
            if (strcmp(method_name, "defineClass1") == 0)
              defined_by_defineClass++;
            else if (strcmp(method_name, "defineAnonymousClass") == 0)
              defined_by_defineAnonymousClass++;
            else {
              fprintf(context_stream, "[Unknown top method!]");
              defined_missing++;
	      read_bytecode = 1;
            }
          } else {
            fprintf(context_stream, "[Unnamed top method!]");
            defined_missing++;
	    read_bytecode = 1;
          }
        }
        printLocation(location, method_id, &read_bytecode, context_stream);

        // Find class that defines the method.
        jclass declaring_class;
        jvmtiError err2 = (*jvmti)->GetMethodDeclaringClass(jvmti, method_id, &declaring_class);
        if (err2 == JVMTI_ERROR_NONE) {
          // Find class signature.
          char* class_sig;
          jvmtiError err3 = (*jvmti)->GetClassSignature(jvmti, declaring_class, &class_sig, NULL);
          if ((err3 == JVMTI_ERROR_NONE) && (class_sig != NULL))
            fprintf(context_stream, "[declaring class: %s]", class_sig);
          else
            fprintf(context_stream, "[declaring class not found (err3).]");
        }
	else
	  fprintf(context_stream, "[declaring class not found (err2).]");
	fprintf(context_stream, " }\n");
	fflush(context_stream);
      }
    }
    if (count == 0) {
      fprintf(context_stream, "[empty stack trace]\n");
      fflush(context_stream);
      defined_by_unknown++;
    }
  }
  free(frames);

  // Sanity check to see if class did not register (or was counted
  // more than once).
  int sum_before = dm + du + dc + dac;
  int sum_after = defined_missing + defined_by_unknown + defined_by_defineClass + defined_by_defineAnonymousClass;
  if ((sum_before + 1) != (sum_after)) {
    fprintf(stderr, "[Class stats check failed: diffs: %d, %d, %d, %d] ", defined_missing - dm,
           defined_by_unknown - du, defined_by_defineClass - dc, defined_by_defineAnonymousClass - dac);
  }

  pthread_mutex_unlock(&stats_lock);

  printClassLoaderInfo(env, loader, context_stream);

  if (use_file != USE_STDOUT)
    fclose(context_stream);
}

void printLoadedClasses(FILE* context_stream) {
  jint class_count;
  jclass* classes;
  jvmtiError err0 = (*jvmti)->GetLoadedClasses(jvmti, &class_count, &classes);
  if (err0 == JVMTI_ERROR_NONE) {
    fprintf(context_stream, "%d loaded classes.\n", class_count);
    for (int i=0; i<class_count; i++) {
      // Find class signature.
      char* class_sig;
      jvmtiError err1 = (*jvmti)->GetClassSignature(jvmti, classes[i], &class_sig, NULL);
      if ((err1 == JVMTI_ERROR_NONE) && (class_sig != NULL)) {
	jint field_count;
	jfieldID* fields;
	jvmtiError err2 = (*jvmti)->GetClassFields(jvmti, classes[i], &field_count, &fields);
	if (err2 == JVMTI_ERROR_NONE)
	  fprintf(context_stream, "[class: %s (%d fields)]\n", class_sig, field_count);
	else
	  fprintf(context_stream, "[class: %s (cannot retrieve fields, error code %d)]\n", class_sig, err2);
	fflush(context_stream);
      }
      else {
	fprintf(context_stream, "[Unknown class.]");
	fflush(context_stream);
      }
    }
  }
}

/* The hook that instruments class loading and captures all generated
   bytecode. */
void JNICALL
ClassFileLoadHook(jvmtiEnv *jvmti_env, JNIEnv *env, jclass class_being_redefined,
        jobject loader, const char* name, jobject protection_domain,
        jint class_data_len, const unsigned char* class_data,
        jint *new_class_data_len, unsigned char** new_class_data) {

  static int anonymous_class_counter = 0;

  pthread_mutex_lock(&print_lock);

  pthread_mutex_lock(&stats_lock);
  defined_sum++;
  pthread_mutex_unlock(&stats_lock);

  char* out_base_dir = "out";
  size_t out_base_dir_len = strlen(out_base_dir);
  char* out_dir;
  int file_mode = USE_FILE;

  // This failure is mostly for diagnostic reasons. If we remove this
  // check, we may end up with same-name classes, as in the case of
  // the anonymous lambda classes.
  if (class_being_redefined != NULL) {
    fprintf(stderr, "Class redefinition is currently not suported.\n");
    exit(-1);
  }

  // If no name is given (e.g. lambdas), then produce an
  // auto-generated name for the .class file name.
  if (name == 0) {

    pthread_mutex_lock(&stats_lock);
    anonymous_class_counter++;
    pthread_mutex_unlock(&stats_lock);

    printf("Anonymous class #%d found.\n", anonymous_class_counter);
    const int anon_name_len = 40;
    char* anon_name = calloc(anon_name_len, 1);
    int r = snprintf(anon_name, anon_name_len, "AnonGeneratedClass_%d", anonymous_class_counter);
    if (r >= anon_name_len) {
      printf("Internal error: too long auto-generated name for anonymous class.");
      exit(-1);
    }
    printf("* Class name: %s\n", anon_name);

    make_dirs(out_base_dir);
    writeClass(anon_name, out_base_dir, class_data_len, class_data);
    writeExecContext(env, anon_name, loader, out_base_dir, out_base_dir, file_mode);
  }
  else {
    // Ignore built-in classes.
    int builtIn = starts_with("java/", name) || starts_with("javax/", name) || starts_with("com/sun", name) || starts_with("sun/", name) || starts_with("jdk/", name);
    if (builtIn) {
      // printf("Ignoring built-in class: %s\n", name);

      pthread_mutex_lock(&stats_lock);
      defined_but_ignored++;
      pthread_mutex_unlock(&stats_lock);

      goto capture_end;
    }

    // If the fully qualified class name contains '/', it contains a
    // package prefix -- create here a subdirectory for it.
    char* lastSlash = strrchr(name, '/');
    if (lastSlash != 0) {
      size_t package_name_len = lastSlash - name;
      char* package_name = alloca(package_name_len + 1);
      memcpy(package_name, name, package_name_len);
      package_name[package_name_len] = '\0';

      out_dir = malloc(out_base_dir_len + package_name_len + 2);
      sprintf(out_dir, "%s/%s", out_base_dir, package_name);
      printf("Saving class %s (package = %s, name = %s) under \"%s\"\n", name, package_name, &lastSlash[1], out_dir);
    }
    else {
      out_dir = out_base_dir;
      printf("Saving class %s under \"%s\"\n", name, out_dir);
    }

    make_dirs(out_dir);
    writeClass(name, out_base_dir, class_data_len, class_data);
    writeExecContext(env, name, loader, out_base_dir, out_dir, file_mode);

    // printLoadedClasses(stdout);
  }

 capture_end:
  pthread_mutex_unlock(&print_lock);
}

/*
static jint init_options(char *options) {
  char* class_name;
  char* from;
  char* to;

  fprintf(stderr, "Agent library loaded with options = %s\n", options);
  if ((class_name = options) != NULL &&
      (from = strchr(class_name, ',')) != NULL && (from[1] != 0)) {
    *from = 0;
    from++;
    if ((to = strchr(from, ',')) != NULL && (to[1] != 0)) {
      *to = 0;
      to++;
      if (strchr(to, ',') == NULL &&
          strlen(to) == strlen(from) &&
          strlen(class_name) > 0 &&
          strlen(to) > 0) {
        CLASS_NAME = class_name;
        FROM = from;
        TO = to;
        fprintf(stderr, "CLASS_NAME = %s, FROM = %s, TO = %s\n",
                CLASS_NAME, FROM, TO);
        return JNI_OK;
      }
    }
  }
  fprintf(stderr,
          "Incorrect options. You need to start the JVM with -agentlib:ClassFileLoadHook=<classname>,<from>,<to>\n"
          "where <classname> is the class you want to hook, <from> is the string in the classfile to be replaced\n"
          "with <to>.  <from> and <to> must have the same length. Example:\n"
          "    @run main/native -agentlib:ClassFileLoadHook=Foo,XXX,YYY ClassFileLoadHookTest\n");
  return JNI_ERR;
}
*/

static jint Agent_Initialize(JavaVM *jvm, char *options, void *reserved) {
  int rc;

  if ((rc = (*jvm)->GetEnv(jvm, (void **)&jvmti, JVMTI_VERSION_1_2)) != JNI_OK) {
    fprintf(stderr, "Unable to create jvmtiEnv, GetEnv failed, error = %d\n", rc);
    return JNI_ERR;
  }
  /* if ((rc = init_options(options)) != JNI_OK) { */
  /*   return JNI_ERR; */
  /* } */

  (void) memset(&callbacks, 0, sizeof(callbacks));
  callbacks.ClassFileLoadHook = &ClassFileLoadHook;
  if ((rc = (*jvmti)->SetEventCallbacks(jvmti, &callbacks, sizeof(callbacks))) != JNI_OK) {
    fprintf(stderr, "SetEventCallbacks failed, error = %d\n", rc);
    return JNI_ERR;
  }

  if ((rc = (*jvmti)->SetEventNotificationMode(jvmti, JVMTI_ENABLE,
                                               JVMTI_EVENT_CLASS_FILE_LOAD_HOOK, NULL)) != JNI_OK) {
    fprintf(stderr, "SetEventNotificationMode failed, error = %d\n", rc);
    return JNI_ERR;
  }

  (void)memset(&caps, 0, sizeof(jvmtiCapabilities));
  caps.can_get_bytecodes = 1;
  caps.can_get_line_numbers = 1;
  caps.can_get_constant_pool = 1;

  jvmtiError caps_err = (*jvmti)->AddCapabilities(jvmti, &caps);
  if (caps_err == JVMTI_ERROR_NONE) {
  }
  else
    printf("Capabilities could not be set, some functionality may be missing.\n");

  for (int i = 0; i < 256; i++)
    bytecodes[i] = 0;
  defined_sum = 0;
  defined_by_unknown = 0;
  defined_by_defineClass = 0;
  defined_by_defineAnonymousClass = 0;
  defined_but_ignored = 0;
  defined_missing = 0;

  return JNI_OK;
}

JNIEXPORT jint JNICALL Agent_OnLoad(JavaVM *jvm, char *options, void *reserved) {
  printf("Selecting extra capabilities...\n");

  return Agent_Initialize(jvm, options, reserved);
}

JNIEXPORT jint JNICALL Agent_OnAttach(JavaVM *jvm, char *options, void *reserved) {
  return Agent_Initialize(jvm, options, reserved);
}

JNIEXPORT void JNICALL Agent_OnUnload(JavaVM *vm) {
  fprintf(stderr, "Agent terminates.\n");

  fprintf(stderr, "Classes defined: %d\n", defined_sum);
  fprintf(stderr, "Classes defined (ignored): %d\n", defined_but_ignored);
  fprintf(stderr, "Classes defined by unknown code (stack trace error or empty): %d\n", defined_by_unknown);
  fprintf(stderr, "Classes defined by defineClass(): %d\n", defined_by_defineClass);
  fprintf(stderr, "Classes defined by defineAnonymousClass(): %d\n", defined_by_defineAnonymousClass);

  fprintf(stderr, "Classes in other methods: %d\n", defined_missing);
  fprintf(stderr, "  Bytecode frequencies in call sites:\n");
  int bytecodes_sum = 0;
  int count = 1;
  for (int i = 0; i < 256; i++)
    if (bytecodes[i] != 0) {
      fprintf(stderr, "  %3d ", count++);
      print_bc(stderr, (unsigned char)i);
      fprintf(stderr, " = %d\n", bytecodes[i]);
      bytecodes_sum += bytecodes[i];
    }
  fprintf(stderr, "  Bytecodes sum = %d\n", bytecodes_sum);

  fprintf(stderr, "Uncounted classes: %d\n", defined_sum - (defined_but_ignored + defined_by_unknown + defined_by_defineClass + defined_by_defineAnonymousClass + defined_missing));
}
