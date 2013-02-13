/* Copyright 2012 The Native Client Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can
 * be found in the LICENSE file.
 *
 * This file provides wrappers to open() to use pre-opened file descriptors
 * for the input bitcode and the output file.
 *
 * It also has the SRPC interfaces, but that should probably be refactored
 * into a separate file.
 */

#if defined(__native_client__) && defined(NACL_SRPC)

#include <argz.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
// Headers which are not properly part of the SDK are included by their
// path in the nacl tree
#include "native_client/src/shared/srpc/nacl_srpc.h"
#ifdef __pnacl__
#include <nacl/pnacl.h>
#endif
#include "SRPCStreamer.h"


#include <string>
#include <map>
#include <vector>

#include "llvm/ADT/OwningPtr.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/system_error.h"


using llvm::MemoryBuffer;
using llvm::StringRef;
using std::string;
using std::map;

#define printerr(...)  fprintf(stderr, __VA_ARGS__)
// printdbg is currently disabled to reduce spew.
#define printdbg(...)

#define ARRAY_SIZE(array) (sizeof array / sizeof array[0])

namespace {

typedef std::vector<std::string> string_vector;

// True if the bitcode to be compiled is for a shared library.
// Used to return to the coordinator.
bool g_bitcode_is_shared_library;
// The soname of the current compilation unit, if it is a shared library.
// Empty string otherwise.
std::string* g_bitcode_soname = NULL;
// The newline separated list of libraries that the current bitcode compilation
// unit depends on.
std::string* g_bitcode_lib_dependencies = NULL;
// The filename used internally for looking up the bitcode file.
char kBitcodeFilename[] = "pnacl.pexe";
// The filename used internally for looking up the object code file.
char kObjectFilename[] = "pnacl.o";
// Object which manages streaming bitcode over SRPC and threading.
SRPCStreamer *srpc_streamer;
// FD of the object file.
int object_file_fd;

}  // namespace

//TODO(dschuff): a little more elegant interface into llc than this?
extern llvm::DataStreamer* NaClBitcodeStreamer;

extern int llc_main(int argc, char **argv);

int GetObjectFileFD() {
  return object_file_fd;
}

void NaClRecordObjectInformation(bool is_shared, const std::string& soname) {
  // This function is invoked to begin recording library information.
  // To make it reentrant, we clean up what might be left over from last time.
  delete g_bitcode_soname;
  delete g_bitcode_lib_dependencies;
  // Then remember the module global information.
  g_bitcode_is_shared_library = is_shared;
  g_bitcode_soname = new std::string(soname);
  g_bitcode_lib_dependencies = new std::string();
}

void NaClRecordSharedLibraryDependency(const std::string& library_name) {
  const std::string& kDelimiterString("\n");
  *g_bitcode_lib_dependencies += (library_name + kDelimiterString);
}

namespace {

int DoTranslate(string_vector* cmd_line_vec, int object_fd) {
  if (cmd_line_vec == NULL) {
    return 1;
  }
  object_file_fd = object_fd;
  // Make an argv array from the input vector.
  size_t argc = cmd_line_vec->size();
  char** argv = new char*[argc];
  for (size_t i = 0; i < argc; ++i) {
    // llc_main will not mutate the command line, so this is safe.
    argv[i] = const_cast<char*>((*cmd_line_vec)[i].c_str());
  }
  argv[argc] = NULL;
  // Call main.
  return llc_main(static_cast<int>(argc), argv);
}

string_vector* CommandLineFromArgz(char* str, size_t str_len) {
  char* entry = str;
  string_vector* vec = new string_vector;
  while (entry != NULL) {
    vec->push_back(entry);
    entry = argz_next(str, str_len, entry);
  }
  // Add fixed arguments to the command line.  These specify the bitcode
  // and object code filenames, removing them from the contract with the
  // coordinator.
  vec->push_back(kBitcodeFilename);
  vec->push_back("-o");
  vec->push_back(kObjectFilename);
  return vec;
}

string_vector* GetDefaultCommandLine() {
  string_vector* command_line = new string_vector;
  size_t i;
  // First, those common to all architectures.
  static const char* common_args[] = { "pnacl_translator",
                                       "-filetype=obj",
                                       kBitcodeFilename,
                                       "-o",
                                       kObjectFilename };
  for (i = 0; i < ARRAY_SIZE(common_args); ++i) {
    command_line->push_back(common_args[i]);
  }
  // Then those particular to a platform.
  static const char* llc_args_x8632[] = { "-march=x86",
                                          "-mcpu=pentium4",
                                          "-mtriple=i686-none-nacl-gnu",
                                          NULL };
  static const char* llc_args_x8664[] = { "-march=x86-64",
                                          "-mcpu=core2",
                                          "-mtriple=x86_64-none-nacl-gnu",
                                          NULL };
  static const char* llc_args_arm[] = { "-mcpu=cortex-a8",
                                        "-mtriple=armv7a-none-nacl-gnueabi",
                                        "-arm-reserve-r9",
                                        "-sfi-disable-cp",
                                        "-sfi-store",
                                        "-sfi-load",
                                        "-sfi-stack",
                                        "-sfi-branch",
                                        "-sfi-data",
                                        "-no-inline-jumptables",
                                        "-float-abi=hard",
                                        NULL };

  const char **llc_args = NULL;
#if defined (__pnacl__)
  switch (__builtin_nacl_target_arch()) {
    case PnaclTargetArchitectureX86_32: {
      llc_args = llc_args_x8632;
      break;
    }
    case PnaclTargetArchitectureX86_64: {
      llc_args = llc_args_x8664;
      break;
    }
    case PnaclTargetArchitectureARM_32: {
      llc_args = llc_args_arm;
      break;
    }
    default:
      printerr("no target architecture match.\n");
      delete command_line;
      command_line = NULL;
      break;
  }
#elif defined (__i386__)
  llc_args = llc_args_x8632;
#elif defined (__x86_64__)
  llc_args = llc_args_x8664;
#else
#error
#endif
  for (i = 0; llc_args[i] != NULL; i++) command_line->push_back(llc_args[i]);
  return command_line;
}

// Data passed from main thread to compile thread.
// Takes ownership of the commandline vector.
class StreamingThreadData {
 public:
  StreamingThreadData(int object_fd, string_vector* cmd_line_vec) :
      object_fd_(object_fd), cmd_line_vec_(cmd_line_vec) {}
  int ObjectFD() const { return object_fd_; }
  string_vector* CmdLineVec() const { return cmd_line_vec_.get(); }
  const int object_fd_;
  const llvm::OwningPtr<string_vector> cmd_line_vec_;
};

void *run_streamed(void *arg) {
  StreamingThreadData* data = reinterpret_cast<StreamingThreadData*>(arg);
  data->CmdLineVec()->push_back("-streaming-bitcode");
  if (DoTranslate(data->CmdLineVec(), data->ObjectFD()) != 0) {
    printerr("DoTranslate failed.\n");
    srpc_streamer->setError();
    return NULL;
  }
  delete data;
  return NULL;
}

// Actually do the work for stream initialization.
void do_stream_init(NaClSrpcRpc *rpc,
                    NaClSrpcArg **in_args,
                    NaClSrpcArg **out_args,
                    NaClSrpcClosure *done,
                    string_vector* command_line_vec) {
  NaClSrpcClosureRunner runner(done);
  rpc->result = NACL_SRPC_RESULT_APP_ERROR;
  srpc_streamer = new SRPCStreamer();
  std::string StrError;
  StreamingThreadData* thread_data = new StreamingThreadData(
      in_args[0]->u.hval, command_line_vec);
  NaClBitcodeStreamer = srpc_streamer->init(run_streamed,
      reinterpret_cast<void *>(thread_data),
      &StrError);
  if (NaClBitcodeStreamer) {
    rpc->result = NACL_SRPC_RESULT_OK;
    out_args[0]->arrays.str = strdup("no error");
  } else {
    out_args[0]->arrays.str = strdup(StrError.c_str());
  }
}

// Invoked by the StreamInit RPC to initialize bitcode streaming over SRPC.
// Under the hood it forks a new thread at starts the llc_main, which sets
// up the compilation and blocks when it tries to start reading the bitcode.
// Input arg is a file descriptor to write the output object file to.
// Returns a string, containing an error message if the call fails.
void stream_init(NaClSrpcRpc *rpc,
                 NaClSrpcArg **in_args,
                 NaClSrpcArg **out_args,
                 NaClSrpcClosure *done) {
  // cmd_line_vec allocated by GetDefaultCommandLine() is freed by the
  // translation thread in run_streamed()
  do_stream_init(rpc, in_args, out_args, done, GetDefaultCommandLine());
}

// Invoked by StreamInitWithCommandLine RPC. Same as stream_init, but
// provides a command line to use instead of the default.
void stream_init_with_command_line(NaClSrpcRpc *rpc,
                                   NaClSrpcArg **in_args,
                                   NaClSrpcArg **out_args,
                                   NaClSrpcClosure *done) {
  char* command_line = in_args[1]->arrays.carr;
  size_t command_line_len = in_args[1]->u.count;
  string_vector* cmd_line_vec =
      CommandLineFromArgz(command_line, command_line_len);
  // cmd_line_vec is freed by the translation thread in run_streamed
  do_stream_init(rpc, in_args, out_args, done, cmd_line_vec);
}

// Invoked by the StreamChunk RPC. Receives a chunk of the bitcode and
// buffers it for later retrieval by the compilation thread.
void stream_chunk(NaClSrpcRpc *rpc,
                 NaClSrpcArg **in_args,
                 NaClSrpcArg **out_args,
                 NaClSrpcClosure *done) {
  NaClSrpcClosureRunner runner(done);
  rpc->result = NACL_SRPC_RESULT_APP_ERROR;
  size_t len = in_args[0]->u.count;
  unsigned char *bytes = reinterpret_cast<unsigned char*>(
      in_args[0]->arrays.carr);
  if (srpc_streamer->gotChunk(bytes, len) != len) {
    return;
  }
  rpc->result = NACL_SRPC_RESULT_OK;
}

// Invoked by the StreamEnd RPC. Waits until the compilation finishes,
// then returns. Returns an int indicating whether the bitcode is a
// shared library, a string with the soname, a string with dependencies,
// and a string which contains an error message if applicable.
void stream_end(NaClSrpcRpc *rpc,
                NaClSrpcArg **in_args,
                NaClSrpcArg **out_args,
                NaClSrpcClosure *done) {
  NaClSrpcClosureRunner runner(done);
  rpc->result = NACL_SRPC_RESULT_APP_ERROR;
  std::string StrError;
  if (srpc_streamer->streamEnd(&StrError)) {
    out_args[3]->arrays.str = strdup(StrError.c_str());
    return;
  }
  out_args[0]->u.ival = g_bitcode_is_shared_library;
  // SRPC deletes the strings returned when the closure is invoked.
  // Therefore we need to use strdup.
  out_args[1]->arrays.str = strdup(g_bitcode_soname->c_str());
  out_args[2]->arrays.str = strdup(g_bitcode_lib_dependencies->c_str());
  rpc->result = NACL_SRPC_RESULT_OK;
}

const struct NaClSrpcHandlerDesc srpc_methods[] = {
  // Protocol for streaming:
  // (StreamInit(obj_fd) -> error_str |
  //    StreamInitWIthCommandLine(obj_fd, escaped_cmdline) -> error_str)
  // StreamChunk(data) +
  // StreamEnd() -> (is_shared_lib,soname,dependencies,error_str)
  { "StreamInit:h:s", stream_init },
  { "StreamInitWithCommandLine:hC:s:", stream_init_with_command_line },
  { "StreamChunk:C:", stream_chunk },
  { "StreamEnd::isss", stream_end },
  { NULL, NULL },
};

}  // namespace

int
main() {
  if (!NaClSrpcModuleInit()) {
    return 1;
  }

  if (!NaClSrpcAcceptClientConnection(srpc_methods)) {
    return 1;
  }
  NaClSrpcModuleFini();
  return 0;
}

#endif
