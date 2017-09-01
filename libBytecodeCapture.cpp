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

#include <string.h>
#include <pthread.h>
#include <sys/stat.h>

#include <fstream>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <string>

#include <jvmti.h>

// Serialize the execution of this agent to account for concurrent
// class loading.
#define SERIALIZE 1

static jvmtiEnv *jvmti = NULL;
static jvmtiEventCallbacks callbacks;
static jvmtiCapabilities caps;
static int bytecodes[256];

static pthread_mutex_t stats_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t serialize_lock = PTHREAD_MUTEX_INITIALIZER;
static int defined_sum;
static int defined_by_defineClass;
static int defined_by_defineAnonymousClass;
static int defined_by_unknown;
static int defined_missing;
static int defined_but_ignored;

// Flags to control output in standard output (slow, must be
// serialized) or files (async).
enum OUTPUT_MODE { USE_STDOUT, USE_FILE };

using namespace std;

inline bool starts_with(const string search_str, const string s) {
  return (s.substr(0, search_str.size()) == search_str);
}

// Given a directory name, this function calls 'mkdir -p' to create
// it, including all its parents.
void make_dirs(const string out_dir) {
  stringstream mkdir_cmd;
  mkdir_cmd << "mkdir -p '" << out_dir << "'";
  system(mkdir_cmd.str().c_str());
}

// Taken from https://stackoverflow.com/questions/12774207/fastest-way-to-check-if-a-file-exist-using-standard-c-c11-c
inline bool file_exists (const std::string& name) {
  struct stat buffer;
  return (stat (name.c_str(), &buffer) == 0);
}

// Writes a bytecode data stream to a file. Takes the fully-qualifed
// name of the class (e.g. 'package1/package2/C'), the base output
// directory (e.g. 'out'), the full output dir for the target class,
// the length of the class data, and the class data byte array.
//
// Returns 0 if the class was saved, 1 if the same class has already
// been saved, 2 if another class with the same name was saved.
int write_class(const string name, const string out_base_dir,
                jint class_data_len, const unsigned char* class_data) {

  stringstream class_file_name_s;
  class_file_name_s << out_base_dir << "/" << name << ".class";
  string class_file_name = class_file_name_s.str();
  if (file_exists(class_file_name)) {
    // Output file already exists, check if its contents are the
    // same or we have another class with the same name.
    // FILE* existing = fopen(class_file_name, "r");
    ifstream existing;
    existing.open(class_file_name, ios::in | ios::binary);
    // fseek(existing, 0L, SEEK_END);
    existing.seekg(0L, ios::end);
    // size_t sz = ftell(existing);
    streampos sz = existing.tellg();
    if (sz != (streampos)class_data_len) {
      cerr <<  "File " << class_file_name <<
        " already exists, with different contents (different size: " << sz <<
        " vs. " << class_data_len<< ")." << endl;
      return 2;
    }
    else {
      existing.seekg(0L, ios::beg);
      char *mem_block = new char[class_data_len];
      existing.read(mem_block, class_data_len);
      // rewind(existing);
      int different_pos = -1;
      for (int pos = 0; pos < class_data_len; pos++)
        if (mem_block[pos] != class_data[pos]) {
          different_pos = pos;
          break;
        }
      // fclose(existing);
      existing.close();
      if (different_pos == -1) {
        cerr << "File " << class_file_name << " already exists, with same contents." << endl;
        return 1;
      }
      else {
        cerr <<  "File " << class_file_name << " already exists, with different contents (first different byte @ pos " << different_pos << " )" << endl;
        exit(-1);
        return 2;
      }
    }
  } else {
    /* // Replace '$' with '_' (e.g. generated proxy classes). */
    /* for (int i = 0; i < strlen(class_file_name); i++) */
    /*   if (class_file_name[i] == '$') */
    /*  class_file_name[i] = '_'; */
    cout << "* Writing " << class_file_name << " (" << class_data_len << " bytes)..." << endl;
    ofstream class_file;
    class_file.open(class_file_name, ios::app | ios::binary);
    class_file.write((const char*)class_data, class_data_len);
    class_file.close();
  }
  return 0;
}

// Call hashCode() method on Java object.
int hash_code(JNIEnv *env, const jobject obj) {
  if (!obj)
    return 0;
  jclass klass = env->GetObjectClass(obj);
  if (NULL == klass) {
    return 0;
  } else {
    jmethodID hashCode_m = env->GetMethodID(klass, "hashCode", "()I");
    return (int)env->CallIntMethod(obj, hashCode_m);
  }
}

void print_classloader_info(ostream* context_stream, JNIEnv *env,
                            const jobject loader, const int loader_hash) {
  if (loader == NULL)
    *context_stream << "[Null classloader (bootstrap?)]" << endl;
  else {
    // Show information about the class loader.
    jclass loader_class = env->GetObjectClass(loader);
    if (loader_class == NULL)
      *context_stream << "[Error retrieving classloader " << loader_hash << " (#1).]" << endl;
    else {
      char* loader_sig;
      jvmtiError err = jvmti->GetClassSignature(loader_class, &loader_sig, NULL);
      if ((err == JVMTI_ERROR_NONE) && (loader_sig != NULL))
        *context_stream << "[classloader " << loader_hash << " class: " << loader_sig << "]" << endl;
      else
        *context_stream << "[Error retrieving classloader " << loader_hash << " (#2).]" << endl;
    }
  }
  context_stream->flush();
}

// Test disassembler of selected bytecode instructions.
void print_bc(ostream* stream, const unsigned char c) {
  switch (c) {
  case  18: *stream << "ldc"            ; break;
  case  19: *stream << "ldc_w"          ; break;
  case 178: *stream << "getstatic"      ; break;
  case 179: *stream << "putstatic"      ; break;
  case 182: *stream << "invokevirtual"  ; break;
  case 183: *stream << "invokespecial"  ; break;
  case 184: *stream << "invokestatic"   ; break;
  case 185: *stream << "invokeinterface"; break;
  case 186: *stream << "invokedynamic"  ; break;
  case 187: *stream << "new"            ; break;
  case 189: *stream << "anewarray"      ; break;
  case 191: *stream << "athrow"         ; break;
  case 192: *stream << "checkcast"      ; break;
  case 193: *stream << "instanceof"     ; break;
  case 197: *stream << "multianewarray" ; break;
  default : *stream << "bytecode-" << c ;
  }
}

void count_bytecode_location(ostream* context_stream, jlocation location,
                             jmethodID method_id) {
  jint bytecode_count_ptr;
  unsigned char* bytecodes_ptr;
  jvmtiError bc_err = jvmti->GetBytecodes(method_id, &bytecode_count_ptr, &bytecodes_ptr);
  if (bc_err == JVMTI_ERROR_NONE) {
    char bc = bytecodes_ptr[location];

    // No lock here, caller holds the lock.
    // pthread_mutex_lock(&stats_lock);
    bytecodes[(unsigned char)bc]++;
    // pthread_mutex_unlock(&stats_lock);

    *context_stream << "[bc:"; print_bc(context_stream, bc); *context_stream << "]";
  }
  else
    *context_stream << "(error reading bytecode)";
}

void print_location(ostream* context_stream, const jlocation location,
                    const jmethodID method_id, int* read_bytecode) {
  jvmtiJlocationFormat locFormat;
  if (location == -1) {
    *context_stream << "(native method) ";
    return;
  }
  jvmtiError err1 = jvmti->GetJLocationFormat(&locFormat);
  if (err1 != JVMTI_ERROR_NONE) {
    *context_stream << "(error reading location) ";
    return;
  }
  else if (locFormat != JVMTI_JLOCATION_JVMBCI) {
    *context_stream << "(unsupported location type) ";
    return;
  }
  else {
    *context_stream << "(bytecode @ position " << location <<  ") ";
    if (*read_bytecode) {
      count_bytecode_location(context_stream, location, method_id);
      *read_bytecode = 0;
    }

    jint entry_count;
    jvmtiLineNumberEntry* table;
    jvmtiError lines_err = jvmti->GetLineNumberTable(method_id, &entry_count, &table);
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
      *context_stream << "(candidate line number: " << table[j-1].line_number << ") ";
      found = 1;
      break;
    }
      }
      if (!found)
        *context_stream << "(could not determine source location) ";
    }
    else
      *context_stream << "(source location: error " << lines_err << ") ";
  }
}

ostream *choose_stdout_or_file(const string class_name, const string out_base_dir,
                               const string out_dir, const int file_mode) {
  if (file_mode == USE_STDOUT)
    return &cout;
  else {
    stringstream ss;
    ss << out_base_dir << "/" << class_name << ".info";
    string info_file_name = ss.str();
    ofstream *context_stream = new ofstream;
    context_stream->open(info_file_name, ios::app);
    return context_stream;
  }
}

void print_declaring_class(ostream* context_stream, const jmethodID method_id) {
  // Find class that defines the method.
  jclass declaring_class;
  jvmtiError err2 = jvmti->GetMethodDeclaringClass(method_id, &declaring_class);
  if (err2 == JVMTI_ERROR_NONE) {
    // Find class signature.
    char* class_sig;
    jvmtiError err3 = jvmti->GetClassSignature(declaring_class, &class_sig, NULL);
    if ((err3 == JVMTI_ERROR_NONE) && (class_sig != NULL))
      *context_stream << "[declaring class: " << class_sig << "]";
    else
      *context_stream << "[declaring class not found (err3).]";
  }
  else
    *context_stream << "[declaring class not found (err2).]";
}

// Reads the stack and finds the innermost method.
void write_exec_context(JNIEnv *env, const string class_name,
                        const jobject loader, const int loader_hash,
                        const string out_base_dir, const string out_dir,
                        const int file_mode) {
  const jint max_frame_count = 47;
  jvmtiFrameInfo* frames = new jvmtiFrameInfo[max_frame_count];
  jint count;
  jthread current_thread = NULL;
  ostream *context_stream = choose_stdout_or_file(class_name, out_base_dir, out_dir, file_mode);

  pthread_mutex_lock(&stats_lock);

  int dc = defined_by_defineClass;
  int dac = defined_by_defineAnonymousClass;
  int du = defined_by_unknown;
  int dm = defined_missing;

  jvmtiError err = jvmti->GetStackTrace(current_thread, 0,
                                        max_frame_count, frames, &count);
  if (err != JVMTI_ERROR_NONE) {
    *context_stream << "[error reading stack trace]";
    context_stream->flush();
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
      err = jvmti->GetMethodName(method_id, &method_name, NULL, &method_sig);
      if (err != JVMTI_ERROR_NONE) {
        defined_by_unknown++;
      } else {
        *context_stream << "{ Frame " << i << ": ";
        // *context_stream << "Class " << class_name << " loaded while executing method: " << method_name << endl;
        *context_stream << "* In method: " << method_name << " (signature: " <<
                           (method_sig == NULL? "no signature" : method_sig) << ") " << endl;
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
              *context_stream << "[Unknown top method!]";
              defined_missing++;
              read_bytecode = 1;
            }
          } else {
            *context_stream << "[Unnamed top method!]";
            defined_missing++;
            read_bytecode = 1;
          }
        }
        print_location(context_stream, location, method_id, &read_bytecode);
        print_declaring_class(context_stream, method_id);
        *context_stream << " }" << endl;
        context_stream->flush();
      }
    }
    if (count == 0) {
      *context_stream << "[empty stack trace]" << endl;
      context_stream->flush();
      defined_by_unknown++;
    }
  }
  free(frames);

  // Sanity check to see if class did not register (or was counted
  // more than once).
  int sum_before = dm + du + dc + dac;
  int sum_after = defined_missing + defined_by_unknown + defined_by_defineClass + defined_by_defineAnonymousClass;
  if ((sum_before + 1) != (sum_after)) {
    cerr << "[Class stats check failed: diffs: " <<
      defined_missing - dm << ", " <<
      defined_by_unknown - du << ", " <<
      defined_by_defineClass - dc << ", " <<
      defined_by_defineAnonymousClass - dac << "] ";
  }

  print_classloader_info(context_stream, env, loader, loader_hash);

  pthread_mutex_unlock(&stats_lock);

  // // TODO: close
  // if (file_mode != USE_STDOUT)
  //   context_stream->close();
}

void printLoadedClasses(ostream* context_stream) {
  jint class_count;
  jclass* classes;
  jvmtiError err0 = jvmti->GetLoadedClasses(&class_count, &classes);
  if (err0 == JVMTI_ERROR_NONE) {
    *context_stream << class_count << " loaded classes." << endl;
    for (int i=0; i<class_count; i++) {
      // Find class signature.
      char* class_sig;
      jvmtiError err1 = jvmti->GetClassSignature(classes[i], &class_sig, NULL);
      if ((err1 == JVMTI_ERROR_NONE) && (class_sig != NULL)) {
        jint field_count;
        jfieldID* fields;
        jvmtiError err2 = jvmti->GetClassFields(classes[i], &field_count, &fields);
        if (err2 == JVMTI_ERROR_NONE)
          *context_stream << "[class: " << class_sig << " (" << field_count << " fields)]" << endl;
        else
          *context_stream << "[class: " << class_sig << " (cannot retrieve fields, error code " << err2 << ")]" << endl;
        context_stream->flush();
      }
      else {
        *context_stream << "[Unknown class.]";
        context_stream->flush();
      }
    }
  }
}

void record_class(JNIEnv *env, const string class_name, const jobject loader,
                  const int loader_hash, const string out_base_dir,
                  const string out_dir, const int file_mode,
                  jint class_data_len, const unsigned char* class_data) {
  make_dirs(out_dir);
  write_class(class_name, out_base_dir, class_data_len, class_data);
  write_exec_context(env, class_name, loader, loader_hash, out_base_dir,
                     out_dir, file_mode);
}

/* The hook that instruments class loading and captures all generated
   bytecode. */
void JNICALL
ClassFileLoadHook(jvmtiEnv *jvmti_env, JNIEnv *env, jclass class_being_redefined,
        jobject loader, const char* name, jobject protection_domain,
        jint class_data_len, const unsigned char* class_data,
        jint *new_class_data_len, unsigned char** new_class_data) {

  static int anonymous_class_counter = 0;

  if (SERIALIZE)
    pthread_mutex_lock(&serialize_lock);

  pthread_mutex_lock(&stats_lock);
  defined_sum++;
  pthread_mutex_unlock(&stats_lock);

  int loader_hash = hash_code(env, loader);
  stringstream ss;
  ss << "out/" << loader_hash;
  string out_base_dir = ss.str();
  string out_dir;
  OUTPUT_MODE file_mode = USE_FILE;

  // This failure is mostly for diagnostic reasons. If we remove this
  // check, we may end up with same-name classes, as in the case of
  // the anonymous lambda classes.
  if (class_being_redefined != NULL) {
    cerr << "Class redefinition is currently not suported." << endl;
    exit(-1);
  }

  // If no name is given (e.g. lambdas), then produce an
  // auto-generated name for the .class file name.
  if (name == 0) {

    pthread_mutex_lock(&stats_lock);
    anonymous_class_counter++;
    pthread_mutex_unlock(&stats_lock);

    cout << "Anonymous class #" << anonymous_class_counter << " found." << endl;
    stringstream anon_name_s;
    anon_name_s << "AnonGeneratedClass_" << anonymous_class_counter;
    string anon_name = anon_name_s.str();
    cout << "* Class name: " << anon_name << endl;

    record_class(env, anon_name, loader, loader_hash, out_base_dir, out_base_dir,
                 file_mode, class_data_len, class_data);
  }
  else {
    string name_s(name);
    // Ignore built-in classes.
    int builtIn = starts_with("java/", name) || starts_with("javax/", name) || starts_with("com/sun", name) || starts_with("sun/", name) || starts_with("jdk/", name);
    if (builtIn) {
      // cout << "Ignoring built-in class: " << name << endl;

      pthread_mutex_lock(&stats_lock);
      defined_but_ignored++;
      pthread_mutex_unlock(&stats_lock);

      goto capture_end;
    }

    // If the fully qualified class name contains '/', it contains a
    // package prefix -- create here a subdirectory for it.
    size_t last_slash_pos = name_s.find_last_of('/');
    if (last_slash_pos != string::npos) {
      string package_name = name_s.substr(0, last_slash_pos);
      string extracted_name = name_s.substr(last_slash_pos + 1, string::npos);
      stringstream out_ss;
      out_ss << out_base_dir << "/" << package_name;
      out_dir = out_ss.str();
      cout << "Saving class " << name << " (package = " << package_name << ", name = " << extracted_name << ") under \"" << out_dir << "\"" << endl;
    }
    else {
      out_dir = out_base_dir;
      cout << "Saving class " << name << " under \"" << out_dir << "\"" << endl;
    }

    record_class(env, name, loader, loader_hash, out_base_dir, out_dir, file_mode,
                 class_data_len, class_data);
    // printLoadedClasses(stdout);
  }

 capture_end:
  if (SERIALIZE)
    pthread_mutex_unlock(&serialize_lock);
  else
    return;
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

  if ((rc = jvm->GetEnv((void **)&jvmti, JVMTI_VERSION_1_2)) != JNI_OK) {
    cerr << "Unable to create jvmtiEnv, GetEnv failed, error = " << rc << endl;
    return JNI_ERR;
  }
  /* if ((rc = init_options(options)) != JNI_OK) { */
  /*   return JNI_ERR; */
  /* } */

  (void) memset(&callbacks, 0, sizeof(callbacks));
  callbacks.ClassFileLoadHook = &ClassFileLoadHook;
  if ((rc = jvmti->SetEventCallbacks(&callbacks, sizeof(callbacks))) != JNI_OK) {
    cerr << "SetEventCallbacks failed, error = " << rc << endl;
    return JNI_ERR;
  }

  if ((rc = jvmti->SetEventNotificationMode(JVMTI_ENABLE,
                                            JVMTI_EVENT_CLASS_FILE_LOAD_HOOK, NULL)) != JNI_OK) {
    cerr << "SetEventNotificationMode failed, error = " << rc << endl;
    return JNI_ERR;
  }

  (void)memset(&caps, 0, sizeof(jvmtiCapabilities));
  caps.can_get_bytecodes = 1;
  caps.can_get_line_numbers = 1;
  caps.can_get_constant_pool = 1;

  jvmtiError caps_err = jvmti->AddCapabilities(&caps);
  if (caps_err == JVMTI_ERROR_NONE) {
  }
  else
    cout << "Capabilities could not be set, some functionality may be missing." << endl;

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
  cout << "Selecting extra capabilities..." << endl;

  return Agent_Initialize(jvm, options, reserved);
}

JNIEXPORT jint JNICALL Agent_OnAttach(JavaVM *jvm, char *options, void *reserved) {
  return Agent_Initialize(jvm, options, reserved);
}

JNIEXPORT void JNICALL Agent_OnUnload(JavaVM *vm) {
  cerr << "Agent terminates." << endl;

  cerr << "Classes defined: " << defined_sum << endl;
  cerr << "Classes defined (ignored): " << defined_but_ignored << endl;
  cerr << "Classes defined by unknown code (stack trace error or empty): " << defined_by_unknown << endl;
  cerr << "Classes defined by defineClass(): " << defined_by_defineClass << endl;
  cerr << "Classes defined by defineAnonymousClass(): " << defined_by_defineAnonymousClass << endl;

  cerr << "Classes in other methods: " << defined_missing << endl;
  cerr << "  Bytecode frequencies in call sites:" << endl;
  int bytecodes_sum = 0;
  int count = 1;
  cerr << setprecision(3);
  for (int i = 0; i < 256; i++)
    if (bytecodes[i] != 0) {
      cerr << "  " << (count++) << " ";
      print_bc(&cerr, (unsigned char)i);
      cerr << " = " << bytecodes[i] << endl;
      bytecodes_sum += bytecodes[i];
    }
  cerr << "  Bytecodes sum = " << bytecodes_sum << endl;

  int uncounted = defined_sum - (defined_but_ignored + defined_by_unknown + defined_by_defineClass + defined_by_defineAnonymousClass + defined_missing);
  cerr << "Uncounted classes: " << uncounted << endl;
}
