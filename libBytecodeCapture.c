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

#include <jvmti.h>

static jvmtiEnv *jvmti = NULL;
static jvmtiEventCallbacks callbacks;

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
  int mkdir_cmd_len = strlen(out_dir) + 11;
  char* mkdir_cmd = alloca(mkdir_cmd_len);
  int r = snprintf(mkdir_cmd, mkdir_cmd_len, "mkdir -p %s", out_dir);
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
void writeClass(const char* name, const char* out_base_dir, const char* out_dir,
		jint class_data_len, const unsigned char* class_data) {
  make_dirs(out_dir);
  
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

void printClassLoaderInfo(JNIEnv *env, const jobject loader) {
  // Show information about the class loader.
  jclass loader_class = (*env)->GetObjectClass(env, loader);
  if (loader_class == NULL) {
    printf("[Error retrieving classloader (#1).]\n");
  }
  else {
    char* loader_sig;
    jvmtiError err = (*jvmti)->GetClassSignature(jvmti, loader_class, &loader_sig, NULL);
    if ((err == JVMTI_ERROR_NONE) && (loader_sig != NULL)) {
      printf("[classloader class: %s]\n", loader_sig);
      fflush(stdout);
    }
    else {
      printf("[Error retrieving classloader (#2).]\n");
      fflush(stdout);
    }
  }
}

// Reads the stack and finds the innermost method.
void writeExecContext(JNIEnv *env, const char* class_name, const jobject loader) {
  jint max_frame_count = 10;
  jvmtiFrameInfo* frames = (jvmtiFrameInfo*)calloc(max_frame_count, sizeof(jvmtiFrameInfo));
  jint count;

  jthread current_thread = NULL;

  jvmtiError err = (*jvmti)->GetStackTrace(jvmti, current_thread, 0,
					   max_frame_count, frames, &count);
  if (err == JVMTI_ERROR_NONE && count >= 1) {
    char *method_name;
    jmethodID method_id = frames[0].method;
    char* method_sig;
    err = (*jvmti)->GetMethodName(jvmti, method_id,
				  &method_name, NULL, &method_sig);
    if (err == JVMTI_ERROR_NONE) {
      // printf("Class %s loaded while executing method: %s\n", class_name, method_name);
      if (method_sig != NULL)
	printf("* In method: %s (signature: %s) ", method_name, method_sig);
      else
	printf("* In method: %s (no signature) ", method_name);

      // Find class that defines the method.
      jclass declaring_class;
      jvmtiError err2 = (*jvmti)->GetMethodDeclaringClass(jvmti, method_id, &declaring_class);
      if (err2 == JVMTI_ERROR_NONE) {
	// Find class signature.
	char* class_sig;
	jvmtiError err3 = (*jvmti)->GetClassSignature(jvmti, declaring_class, &class_sig, NULL);
	if ((err3 == JVMTI_ERROR_NONE) && (class_sig != NULL))
	  printf("[declaring class: %s]", class_sig);
	else
	  printf("[declaring class not found (err3).]");
      }
      else
	printf("[declaring class not found (err2).]");
      fflush(stdout);
    }
  }
  else {
    printf("[No executing method!]");
    fflush(stdout);
  }
  free(frames);
  printClassLoaderInfo(env, loader);
}

void printLoadedClasses() {
  jint class_count;
  jclass* classes;
  jvmtiError err0 = (*jvmti)->GetLoadedClasses(jvmti, &class_count, &classes);
  if (err0 == JVMTI_ERROR_NONE) {
    printf("%d loaded classes.\n", class_count);
    for (int i=0; i<class_count; i++) {
      // Find class signature.
      char* class_sig;
      jvmtiError err1 = (*jvmti)->GetClassSignature(jvmti, classes[i], &class_sig, NULL);
      if ((err1 == JVMTI_ERROR_NONE) && (class_sig != NULL)) {
	jint field_count;
	jfieldID* fields;
	jvmtiError err2 = (*jvmti)->GetClassFields(jvmti, classes[i], &field_count, &fields);
	if (err2 == JVMTI_ERROR_NONE)
	  printf("[class: %s (%d fields)]\n", class_sig, field_count);
	else
	  printf("[class: %s (cannot retrieve fields, error code %d)]\n", class_sig, err2);
	fflush(stdout);
      }
      else {
	printf("[Unknown class.]");
	fflush(stdout);
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

  char* out_base_dir = "out";
  size_t out_base_dir_len = strlen(out_base_dir);
  char* out_dir;

  // This failure is mostly for diagnostic reasons. If we remove this
  // check, we may end up with same-name classes, as in the case of
  // the anonymous lambda classes.
  if (class_being_redefined != NULL) {
    printf("Class redefinition is currently not suported.\n");
    exit(-1);
  }

  // If no name is given (e.g. lambdas), then produce an
  // auto-generated name for the .class file name.
  if (name == 0) {
    anonymous_class_counter++;
    printf("Anonymous class #%d found.\n", anonymous_class_counter);
    const int anon_name_len = 40;
    char* anon_name = calloc(anon_name_len, 1);
    int r = snprintf(anon_name, anon_name_len, "AnonGeneratedClass_%d", anonymous_class_counter);
    if (r >= anon_name_len) {
      printf("Internal error: too long auto-generated name for anonymous class.");
      exit(-1);
    }
    printf("* Class name: %s\n", anon_name);
    writeClass(anon_name, out_base_dir, out_base_dir, class_data_len, class_data);
    writeExecContext(env, anon_name, loader);
  }
  else {
    // Ignore built-in classes.
    int builtIn = starts_with("java/", name) || starts_with("javax/", name) || starts_with("com/sun", name) || starts_with("sun/", name) || starts_with("jdk/", name);
    if (builtIn) {
      // printf("Ignoring built-in class: %s\n", name);
      // writeExecContext(name);
      return;
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
    writeClass(name, out_base_dir, out_dir, class_data_len, class_data);
    writeExecContext(env, name, loader);

    printLoadedClasses();
  }
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

  return JNI_OK;
}

JNIEXPORT jint JNICALL Agent_OnLoad(JavaVM *jvm, char *options, void *reserved) {
  return Agent_Initialize(jvm, options, reserved);
}

JNIEXPORT jint JNICALL Agent_OnAttach(JavaVM *jvm, char *options, void *reserved) {
  return Agent_Initialize(jvm, options, reserved);
}
