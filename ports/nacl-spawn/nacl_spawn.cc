// Copyright (c) 2014 The Native Client Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Emulates spawning/waiting process by asking JavaScript to do so.

// Include quoted spawn.h first so we can build in the presence of an installed
// copy of nacl-spawn.
#define IN_NACL_SPAWN_CC
#include "spawn.h"

#include "nacl_main.h"

#include <netinet/in.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <locale.h>
#include <irt.h>
#include <irt_dev.h>
#include <netinet/in.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "ppapi/c/ppb_file_system.h"
#include "ppapi/c/ppb_var.h"
#include "ppapi/c/ppb_var_array.h"
#include "ppapi/c/ppb_var_dictionary.h"

#include "ppapi_simple/ps.h"
#include "ppapi_simple/ps_instance.h"
#include "ppapi_simple/ps_interface.h"

#include "nacl_io/nacl_io.h"
#include "nacl_io/fuse.h"

#include "library_dependencies.h"
#include "path_util.h"


#define MAX_OLD_PIPES 100


extern char** environ;

int nacl_spawn_pid;
int nacl_spawn_ppid;

struct NaClSpawnReply {
  pthread_mutex_t mu;
  pthread_cond_t cond;

  struct PP_Var result_var;
};

static void VarAddRef(struct PP_Var var) {
  PSInterfaceVar()->AddRef(var);
}

static void VarRelease(struct PP_Var var) {
  PSInterfaceVar()->Release(var);
}

static struct PP_Var VarDictionaryCreate(void) {
  struct PP_Var ret = PSInterfaceVarDictionary()->Create();
  return ret;
}

static bool VarDictionaryHasKey(struct PP_Var dict,
                                const char* key,
                                struct PP_Var* out_value) {
  assert(out_value);
  struct PP_Var key_var = PSInterfaceVar()->VarFromUtf8(key, strlen(key));
  bool has_value = PSInterfaceVarDictionary()->HasKey(dict, key_var);
  if (has_value) {
    *out_value = PSInterfaceVarDictionary()->Get(dict, key_var);
  }
  PSInterfaceVar()->Release(key_var);
  return has_value;
}

static struct PP_Var VarDictionaryGet(struct PP_Var dict, const char* key) {
  struct PP_Var key_var = PSInterfaceVar()->VarFromUtf8(key, strlen(key));
  struct PP_Var ret = PSInterfaceVarDictionary()->Get(dict, key_var);
  PSInterfaceVar()->Release(key_var);
  return ret;
}

static void VarDictionarySet(struct PP_Var dict,
                             const char* key,
                             struct PP_Var value_var) {
  struct PP_Var key_var = PSInterfaceVar()->VarFromUtf8(key, strlen(key));
  PSInterfaceVarDictionary()->Set(dict, key_var, value_var);
  PSInterfaceVar()->Release(key_var);
  PSInterfaceVar()->Release(value_var);
}

static void VarDictionarySetString(struct PP_Var dict,
                                   const char* key,
                                   const char* value) {
  struct PP_Var value_var = PSInterfaceVar()->VarFromUtf8(value, strlen(value));
  VarDictionarySet(dict, key, value_var);
}

static struct PP_Var VarArrayCreate(void) {
  struct PP_Var ret = PSInterfaceVarArray()->Create();
  return ret;
}

static void VarArrayInsert(struct PP_Var array,
                           uint32_t index,
                           struct PP_Var value_var) {
  uint32_t old_length = PSInterfaceVarArray()->GetLength(array);
  PSInterfaceVarArray()->SetLength(array, old_length + 1);

  for (uint32_t i = old_length; i > index; --i) {
    struct PP_Var from_var = PSInterfaceVarArray()->Get(array, i - 1);
    PSInterfaceVarArray()->Set(array, i, from_var);
    PSInterfaceVar()->Release(from_var);
  }
  PSInterfaceVarArray()->Set(array, index, value_var);
  PSInterfaceVar()->Release(value_var);
}

static void VarArraySetString(struct PP_Var array,
                              uint32_t index,
                              const char* value) {
  struct PP_Var value_var = PSInterfaceVar()->VarFromUtf8(value, strlen(value));
  PSInterfaceVarArray()->Set(array, index, value_var);
  PSInterfaceVar()->Release(value_var);
}

static void VarArrayInsertString(struct PP_Var array,
                                 uint32_t index,
                                 const char* value) {
  struct PP_Var value_var = PSInterfaceVar()->VarFromUtf8(value, strlen(value));
  VarArrayInsert(array, index, value_var);
}

static void VarArrayAppendString(struct PP_Var array,
                                 const char* value) {
  uint32_t index = PSInterfaceVarArray()->GetLength(array);
  VarArraySetString(array, index, value);
}

static void SetInt(struct PP_Var dict_var, const char* key, int32_t v) {
  VarDictionarySet(dict_var, key, PP_MakeInt32(v));
}

static int GetInt(struct PP_Var dict_var, const char* key) {
  struct PP_Var value_var;
  if (!VarDictionaryHasKey(dict_var, key, &value_var)) {
    return -1;
  }
  assert(value_var.type == PP_VARTYPE_INT32);
  int value = value_var.value.as_int;
  if (value < 0) {
    errno = -value;
    return -1;
  }
  return value;
}

static int GetIntAndRelease(struct PP_Var dict_var, const char* key) {
  int ret = GetInt(dict_var, key);
  VarRelease(dict_var);
  return ret;
}

// Get an environment variable as an int, or return -1 if the value cannot
// be converted to an int.
static int getenv_as_int(const char *env) {
  const char* env_str = getenv(env);
  if (!env_str) {
    return -1;
  }
  errno = 0;
  int env_int = strtol(env_str, NULL, 0);
  if (errno) {
    return -1;
  }
  return env_int;
}

static int mkdir_checked(const char* dir) {
  int rtn =  mkdir(dir, S_IRWXU | S_IRWXG | S_IRWXO);
  if (rtn != 0) {
    fprintf(stderr, "mkdir '%s' failed: %s\n", dir, strerror(errno));
  }
  return rtn;
}

static int do_mount(const char *source, const char *target,
                    const char *filesystemtype, unsigned long mountflags,
                    const void *data) {
  NACL_LOG("mount[%s] '%s' at '%s'\n", filesystemtype, source, target);
  return mount(source, target, filesystemtype, mountflags, data);
}

// Returns a unique request ID to make all request strings different
// from each other.
static std::string GetRequestId() {
  static int64_t req_id = 0;
  static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
  pthread_mutex_lock(&mu);
  int64_t id = ++req_id;
  pthread_mutex_unlock(&mu);
  char buf[64];
  sprintf(buf, "%lld", id);
  return buf;
}

static bool GetBool(struct PP_Var dict_var, const char* key) {
  struct PP_Var value_var;
  if (!VarDictionaryHasKey(dict_var, key, &value_var)) {
    return -1;
  }
  assert(value_var.type == PP_VARTYPE_BOOL);
  bool value = value_var.value.as_bool;
  return value;
}

static void MountLocalFs(struct PP_Var mount_data) {
  bool available = GetBool(mount_data, "available");

  if (!available) {
    return;
  } else {
    PP_Var filesystem = VarDictionaryGet(mount_data, "filesystem");
    PP_Resource filesystemResource =
        PSInterfaceVar()->VarToResource(filesystem);
    PP_Var filepath_var = VarDictionaryGet(mount_data, "fullPath");
    PP_Var mountpoint_var = VarDictionaryGet(mount_data, "mountPoint");

    uint32_t fp_len, mp_len;
    const char* filepath = PSInterfaceVar()->VarToUtf8(filepath_var,
                                                       &fp_len);
    const char* mountpoint = PSInterfaceVar()->VarToUtf8(mountpoint_var,
                                                         &mp_len);
    // TODO(gdeepti): Currently mount on the main thread always returns
    // without an error, crashes the nacl module and mkdir in /mnt/html5
    // does not work because we do not allow blocking calls on the main thread.
    // Move this off the main thread for better error checking.
    struct stat st;
    if (stat(mountpoint, &st) < 0) {
      mkdir_checked(mountpoint);
    }

    struct PP_Var status_var = VarDictionaryCreate();
    char fs[1024];
    sprintf(fs, "filesystem_resource=%d\n", filesystemResource);

    if (do_mount(filepath, mountpoint, "html5fs", 0, fs) != 0) {
      fprintf(stderr, "Mounting HTML5 filesystem in %s failed.\n", filepath);
      VarDictionarySetString(status_var, "mount_status", "fail");
    } else {
      VarDictionarySetString(status_var, "mount_status", "success");
    }
    PSInterfaceMessaging()->PostMessage(PSGetInstanceId(), status_var);
    VarRelease(filesystem);
    VarRelease(filepath_var);
    VarRelease(mountpoint_var);
  }
}

static void UnmountLocalFs(struct PP_Var mount_data) {
  bool mounted = GetBool(mount_data, "mounted");
  PP_Var mountpoint_var = VarDictionaryGet(mount_data, "mountPoint");
  uint32_t mp_len;
  const char* mountpoint = PSInterfaceVar()->VarToUtf8(mountpoint_var, &mp_len);

  if(!mounted) {
    perror("Directory not mounted, unable to unmount");
  } else {
    struct PP_Var status_var = VarDictionaryCreate();
    if(umount(mountpoint) != 0) {
      fprintf(stderr, "Unmounting filesystem %s failed.\n", mountpoint);
      VarDictionarySetString(status_var, "unmount_status", "fail");
    } else {
      VarDictionarySetString(status_var, "unmount_status", "success");
    }
    PSInterfaceMessaging()->PostMessage(PSGetInstanceId(), status_var);
    VarRelease(status_var);
    VarRelease(mountpoint_var);
  }
}

// Handle reply from JavaScript. The key is the request string and the
// value is Zero or positive on success or -errno on failure. The
// user_data must be an instance of NaClSpawnReply.
static void HandleNaClSpawnReply(struct PP_Var key,
                                 struct PP_Var value,
                                 void* user_data) {
  if (key.type != PP_VARTYPE_STRING || value.type != PP_VARTYPE_DICTIONARY) {
    fprintf(stderr, "Invalid parameter for HandleNaClSpawnReply\n");
    fprintf(stderr, "key type=%d\n", key.type);
    fprintf(stderr, "value type=%d\n", value.type);
  }
  assert(key.type == PP_VARTYPE_STRING);
  assert(value.type == PP_VARTYPE_DICTIONARY);

  NaClSpawnReply* reply = static_cast<NaClSpawnReply*>(user_data);
  pthread_mutex_lock(&reply->mu);

  VarAddRef(value);
  reply->result_var = value;

  pthread_cond_signal(&reply->cond);
  pthread_mutex_unlock(&reply->mu);
}

static void HandleMountMessage(struct PP_Var key,
                                 struct PP_Var value,
                                 void* user_data) {
  if (key.type != PP_VARTYPE_STRING || value.type != PP_VARTYPE_DICTIONARY) {
    fprintf(stderr, "Invalid parameter for HandleNaClSpawnReply\n");
    fprintf(stderr, "key type=%d\n", key.type);
    fprintf(stderr, "value type=%d\n", value.type);
    return;
  }

  MountLocalFs(value);
}


static void HandleUnmountMessage(struct PP_Var key,
                                 struct PP_Var value,
                                 void* user_data) {
  if (key.type != PP_VARTYPE_STRING || value.type != PP_VARTYPE_DICTIONARY) {
    fprintf(stderr, "Invalid parameter for HandleNaClSpawnReply\n");
    fprintf(stderr, "key type=%d\n", key.type);
    fprintf(stderr, "value type=%d\n", value.type);
    return;
  }

  UnmountLocalFs(value);
}

// Sends a spawn/wait request to JavaScript and returns the result.
static struct PP_Var SendRequest(struct PP_Var req_var) {
  const std::string& req_id = GetRequestId();
  VarDictionarySetString(req_var, "id", req_id.c_str());

  NaClSpawnReply reply;
  pthread_mutex_init(&reply.mu, NULL);
  pthread_cond_init(&reply.cond, NULL);
  PSEventRegisterMessageHandler(req_id.c_str(), &HandleNaClSpawnReply, &reply);

  PSInterfaceMessaging()->PostMessage(PSGetInstanceId(), req_var);
  VarRelease(req_var);

  pthread_mutex_lock(&reply.mu);
  pthread_cond_wait(&reply.cond, &reply.mu);
  pthread_mutex_unlock(&reply.mu);

  pthread_cond_destroy(&reply.cond);
  pthread_mutex_destroy(&reply.mu);

  PSEventRegisterMessageHandler(req_id.c_str(), NULL, &reply);

  return reply.result_var;
}

static void restore_pipes(void) {
  int old_pipes[MAX_OLD_PIPES][3];
  int old_pipe_count = 0;
  int count = 0;
  int i;
  for (;;) {
    char entry[100];
    snprintf(entry, sizeof entry, "NACL_SPAWN_FD_SETUP_%d", count++);
    const char *env_entry = getenv(entry);
    if (!env_entry) {
      break;
    }
    int fd, port, writer;
    if (sscanf(env_entry, "pipe:%d:%d:%d", &fd, &port, &writer) != 3) {
      unsetenv(entry);
      continue;
    }
    unsetenv(entry);
    // NOTE: This is necessary as the javascript assumes all instances
    // of an anonymous pipe will be from the same file object.
    // This allows nacl_io to do the reference counting.
    // naclprocess.js then merely tracks which processes are readers and
    // writers for a given pipe.
    for (i = 0; i < old_pipe_count; ++i) {
      if (old_pipes[i][0] == port && old_pipes[i][1] == writer) {
        dup2(old_pipes[i][2], fd);
        break;
      }
    }
    if (i != old_pipe_count) continue;
    char path[100];
    sprintf(path, "/apipe/%d", port);
    int fd_tmp = open(path, (writer ? O_WRONLY : O_RDONLY));
    if (fd_tmp < 0) {
      fprintf(stderr, "Failed to created pipe on port %d\n", port);
      exit(1);
    }
    if (fd_tmp != fd) {
      dup2(fd_tmp, fd);
      close(fd_tmp);
    }
    if (old_pipe_count >= MAX_OLD_PIPES) {
      fprintf(stderr, "Too many old pipes to restore!\n");
      exit(1);
    }
    old_pipes[old_pipe_count][0] = port;
    old_pipes[old_pipe_count][1] = writer;
    old_pipes[old_pipe_count][2] = fd;
    ++old_pipe_count;
  }
}

static int apipe_open(
    const char* path,
    struct fuse_file_info* info) {
  int id;
  if (sscanf(path, "/%d", &id) != 1) {
    return -ENOENT;
  }
  info->fh = id;
  info->nonseekable = 1;
  return 0;
}

static int apipe_read(
    const char* path, char* buf, size_t count, off_t offset,
    struct fuse_file_info* info) {
  struct PP_Var req_var = VarDictionaryCreate();
  VarDictionarySetString(req_var, "command", "nacl_apipe_read");
  SetInt(req_var, "pipe_id", info->fh);
  SetInt(req_var, "count", count);

  struct PP_Var result_var = SendRequest(req_var);
  struct PP_Var data = VarDictionaryGet(result_var, "data");
  assert(data.type == PP_VARTYPE_ARRAY_BUFFER);
  uint32_t len;
  if(!PSInterfaceVarArrayBuffer()->ByteLength(data, &len)) {
    VarRelease(data);
    VarRelease(result_var);
    return -EIO;
  }
  void *p = PSInterfaceVarArrayBuffer()->Map(data);
  if (len > 0 && !p) {
    VarRelease(data);
    VarRelease(result_var);
    return -EIO;
  }
  assert(len <= count);
  memcpy(buf, p, len);
  PSInterfaceVarArrayBuffer()->Unmap(data);
  VarRelease(data);
  VarRelease(result_var);

  return len;
}

static int apipe_write(
    const char* path,
    const char* buf,
    size_t count,
    off_t,
    struct fuse_file_info* info) {
  if (count == 0) return 0;

  struct PP_Var req_var = VarDictionaryCreate();
  VarDictionarySetString(req_var, "command", "nacl_apipe_write");
  SetInt(req_var, "pipe_id", info->fh);
  struct PP_Var data = PSInterfaceVarArrayBuffer()->Create(count);
  if (data.type == PP_VARTYPE_NULL) return -EIO;
  void *p = PSInterfaceVarArrayBuffer()->Map(data);
  if (count > 0 && !p) {
    VarRelease(data);
    VarRelease(req_var);
    return -EIO;
  }
  memcpy(p, buf, count);
  PSInterfaceVarArrayBuffer()->Unmap(data);
  VarDictionarySet(req_var, "data", data);

  struct PP_Var result_var = SendRequest(req_var);
  int ret = GetInt(result_var, "count");
  VarRelease(result_var);

  return ret;
}

static int apipe_release(const char* path, struct fuse_file_info* info) {
  struct PP_Var req_var = VarDictionaryCreate();
  VarDictionarySetString(req_var, "command", "nacl_apipe_close");
  SetInt(req_var, "pipe_id", info->fh);
  SetInt(req_var, "writer", info->flags == O_WRONLY);

  struct PP_Var result_var = SendRequest(req_var);
  int ret = GetInt(result_var, "result");
  VarRelease(result_var);

  return ret;
}

static int apipe_fgetattr(
    const char* path, struct stat* st, struct fuse_file_info* info) {
  memset(st, 0, sizeof(*st));
  st->st_ino = info->fh;
  st->st_mode = S_IFIFO | S_IRUSR | S_IWUSR;
  // TODO(bradnelson): Do something better.
  // Stashing away the open flags (not a great place).
  st->st_rdev = info->flags;
  return 0;
}

static struct fuse_operations anonymous_pipe_ops;

static void setup_anonymous_pipes(void) {
  const char fs_type[] = "anonymous_pipe";
  int result;

  anonymous_pipe_ops.open = apipe_open;
  anonymous_pipe_ops.read = apipe_read;
  anonymous_pipe_ops.write = apipe_write;
  anonymous_pipe_ops.release = apipe_release;
  anonymous_pipe_ops.fgetattr = apipe_fgetattr;

  result = nacl_io_register_fs_type(fs_type, &anonymous_pipe_ops);
  if (!result) {
    fprintf(stderr, "Error registering filesystem type %s.\n", fs_type);
    exit(1);
  }
  mkdir_checked("/apipe");
  result = do_mount("", "/apipe", fs_type, 0, NULL);
  if (result != 0) {
    fprintf(stderr, "Error mounting %s.\n", fs_type);
    exit(1);
  }
}

static std::string GetCwd() {
  char cwd[PATH_MAX] = ".";
  if (!getcwd(cwd, PATH_MAX)) {
    NACL_LOG("getcwd failed: %s\n", strerror(errno));
    assert(0);
  }
  return cwd;
}

static std::string GetAbsPath(const std::string& path) {
  assert(!path.empty());
  if (path[0] == '/')
    return path;
  else
    return GetCwd() + '/' + path;
}

// Adds a file into nmf. |key| is the key for open_resource IRT or
// "program". |filepath| is not a URL yet. JavaScript code is
// responsible to fix them. |arch| is the architecture string.
static void AddFileToNmf(const std::string& key,
                         const std::string& arch,
                         const std::string& filepath,
                         struct PP_Var dict_var) {

  struct PP_Var url_dict_var = VarDictionaryCreate();
  VarDictionarySetString(url_dict_var, "url", filepath.c_str());

  struct PP_Var arch_dict_var = VarDictionaryCreate();
  VarDictionarySet(arch_dict_var, arch.c_str(), url_dict_var);

  VarDictionarySet(dict_var, key.c_str(), arch_dict_var);
}

static void AddNmfToRequestForShared(
    std::string prog,
    const std::string& arch,
    const std::vector<std::string>& dependencies,
    struct PP_Var req_var) {
  struct PP_Var nmf_var = VarDictionaryCreate();
  struct PP_Var files_var = VarDictionaryCreate();
  const char* prog_base = basename(&prog[0]);
  for (size_t i = 0; i < dependencies.size(); i++) {
    std::string dep = dependencies[i];
    const std::string& abspath = GetAbsPath(dep);
    const char* base = basename(&dep[0]);
    // nacl_helper does not pass the name of program and the dynamic
    // loader always uses "main.nexe" as the main binary.
    if (strcmp(prog_base, base) == 0)
      base = "main.nexe";
    if (strcmp(base, "runnable-ld.so") == 0) {
      AddFileToNmf("program", arch, abspath, nmf_var);
    } else {
      AddFileToNmf(base, arch, abspath, files_var);
    }
  }

  VarDictionarySet(nmf_var, "files", files_var);
  VarDictionarySet(req_var, "nmf", nmf_var);
}

static void AddNmfToRequestForStatic(const std::string& prog,
                                     const std::string& arch,
                                     struct PP_Var req_var) {
  struct PP_Var nmf_var = VarDictionaryCreate();
  AddFileToNmf("program", arch, GetAbsPath(prog), nmf_var);
  VarDictionarySet(req_var, "nmf", nmf_var);
}

static void AddNmfToRequestForPNaCl(const std::string& prog,
                                    struct PP_Var req_var) {
  struct PP_Var url_dict_var = VarDictionaryCreate();
  VarDictionarySetString(url_dict_var, "url", GetAbsPath(prog).c_str());

  struct PP_Var translate_dict_var = VarDictionaryCreate();
  VarDictionarySet(translate_dict_var, "pnacl-translate", url_dict_var);

  struct PP_Var arch_dict_var = VarDictionaryCreate();
  VarDictionarySet(arch_dict_var, "portable", translate_dict_var);

  struct PP_Var nmf_var = VarDictionaryCreate();
  VarDictionarySet(nmf_var, "program", arch_dict_var);

  VarDictionarySet(req_var, "nmf", nmf_var);
}

static void FindInterpreter(std::string* path) {
  // Check if the path exists.
  if (access(path->c_str(), R_OK) == 0) {
    return;
  }
  // As /bin and /usr/bin are currently only mounted to a memory filesystem
  // in nacl_spawn, programs usually located there are installed to some other
  // location which is included in the PATH.
  // For now, do something non-standard.
  // If the program cannot be found at its full path, strip the program path
  // down to the basename and relying on later path search steps to find the
  // actual program location.
  size_t i = path->find_last_of('/');
  if (i == std::string::npos) {
    return;
  }
  *path = path->substr(i + 1);
}

static bool ExpandShBang(std::string* prog, struct PP_Var req_var) {
  // Open script.
  int fh = open(prog->c_str(), O_RDONLY);
  if (fh < 0) {
    return false;
  }
  // Read first 4k.
  char buffer[4096];
  ssize_t len = read(fh, buffer, sizeof buffer);
  if (len < 0) {
    close(fh);
    return false;
  }
  // Close script.
  if (close(fh) < 0) {
    return false;
  }
  // At least must have room for #!.
  if (len < 2) {
    errno = ENOEXEC;
    return false;
  }
  // Check if it's a script.
  if (memcmp(buffer, "#!", 2) != 0) {
    // Not a script.
    return true;
  }
  const char* start = buffer + 2;
  // Find the end of the line while also looking for the first space.
  // Mimicking Linux behavior, in which the first space marks a split point
  // where everything before is the interpreter path and everything after is
  // (including spaces) is treated as a single extra argument.
  const char* end = start;
  const char* split = NULL;
  while (buffer - end < len && *end != '\n' && *end != '\r') {
    if (*end == ' ' && split == NULL) {
      split = end;
    }
    ++end;
  }
  // Update command to run.
  struct PP_Var args_var = VarDictionaryGet(req_var, "args");
  assert(args_var.type == PP_VARTYPE_ARRAY);
  // Set argv[0] in case it was path expanded.
  VarArraySetString(args_var, 0, prog->c_str());
  std::string interpreter;
  if (split) {
    interpreter = std::string(start, split - start);
    std::string arg(split + 1, end - (split + 1));
    VarArrayInsertString(args_var, 0, arg.c_str());
  } else {
    interpreter = std::string(start, end - start);
  }
  FindInterpreter(&interpreter);
  VarArrayInsertString(args_var, 0, interpreter.c_str());
  VarRelease(args_var);
  *prog = interpreter;
  return true;
}

static bool UseBuiltInFallback(std::string* prog, struct PP_Var req_var) {
  if (prog->find('/') == std::string::npos) {
    const char* path_env = getenv("PATH");
    std::vector<std::string> paths;
    GetPaths(path_env, &paths);
    if (GetFileInPaths(*prog, paths, prog)) {
      // Update argv[0] to match prog if we ended up changing it.
      struct PP_Var args_var = VarDictionaryGet(req_var, "args");
      assert(args_var.type == PP_VARTYPE_ARRAY);
      VarArraySetString(args_var, 0, prog->c_str());
      VarRelease(args_var);
    } else {
      // If the path does not contain a slash and we cannot find it
      // from PATH, we use NMF served with the JavaScript.
      return true;
    }
  }
  return false;
}

// Check if a file is a pnacl type file.
// If the file can't be read, return false.
static bool IsPNaClType(const std::string& filename) {
  // Open script.
  int fh = open(filename.c_str(), O_RDONLY);
  if (fh < 0) {
    // Default to nacl type if the file can't be read.
    return false;
  }
  // Read first 4 bytes.
  char buffer[4];
  ssize_t len = read(fh, buffer, sizeof buffer);
  close(fh);
  // Decide based on the header.
  return len == 4 && memcmp(buffer, "PEXE", sizeof buffer) == 0;
}

// Adds a NMF to the request if |prog| is stored in HTML5 filesystem.
static bool AddNmfToRequest(std::string prog, struct PP_Var req_var) {
  if (UseBuiltInFallback(&prog, req_var)) {
    return true;
  }
  if (access(prog.c_str(), R_OK) != 0) {
    errno = ENOENT;
    return false;
  }

  if (!ExpandShBang(&prog, req_var)) {
    return false;
  }

  // Check fallback again in case of #! expanded to something else.
  if (UseBuiltInFallback(&prog, req_var)) {
    return true;
  }

  // Check for pnacl.
  if (IsPNaClType(prog)) {
    AddNmfToRequestForPNaCl(prog, req_var);
    return true;
  }

  std::string arch;
  std::vector<std::string> dependencies;
  if (!FindArchAndLibraryDependencies(prog, &arch, &dependencies))
    return false;
  if (!dependencies.empty()) {
    AddNmfToRequestForShared(prog, arch, dependencies, req_var);
    return true;
  }
  // No dependencies means the main binary is statically linked.
  AddNmfToRequestForStatic(prog, arch, req_var);
  return true;
}

static pid_t waitpid_impl(int pid, int* status, int options);

// TODO(bradnelson): Add sysconf means to query this in all libc's.
#define MAX_FILE_DESCRIPTOR 1000

static int CloneFileDescriptors(struct PP_Var envs_var) {
  int fd;
  int count = 0;

  for (fd = 0; fd < MAX_FILE_DESCRIPTOR; ++fd) {
    struct stat st;
    if (fstat(fd, &st) < 0) {
      if (errno == EBADF) {
        continue;
      }
      return -1;
    }
    int flags = fcntl(fd, F_GETFD);
    if (flags < 0) {
      fprintf(stderr, "fcntl failed when spawning on descriptor %d: %s\n",
          fd, strerror(errno));
      return -1;
    }
    // Skip close on exec descriptors.
    if (flags & FD_CLOEXEC) {
      continue;
    }
    if (S_ISREG(st.st_mode)) {
      // TODO(bradnelson): Land nacl_io ioctl to support this.
    } else if (S_ISDIR(st.st_mode)) {
      // TODO(bradnelson): Land nacl_io ioctl to support this.
    } else if (S_ISCHR(st.st_mode)) {
      // Unsupported.
    } else if (S_ISBLK(st.st_mode)) {
      // Unsupported.
    } else if (S_ISFIFO(st.st_mode)) {
      char entry[100];
      snprintf(entry, sizeof entry,
          "NACL_SPAWN_FD_SETUP_%d=pipe:%d:%d:%d", count++, fd,
          static_cast<int>(st.st_ino), st.st_rdev == O_WRONLY);
      VarArrayAppendString(envs_var, entry);
    } else if (S_ISLNK(st.st_mode)) {
      // Unsupported.
    } else if (S_ISSOCK(st.st_mode)) {
      // Unsupported.
    }
  }
  return 0;
}

static void stash_file_descriptors(void) {
  int fd;

  for (fd = 0; fd < MAX_FILE_DESCRIPTOR; ++fd) {
    // TODO(bradnelson): Make this more robust if there are more than
    // MAX_FILE_DESCRIPTOR descriptors.
    if (dup2(fd, fd + MAX_FILE_DESCRIPTOR) < 0) {
      assert(errno == EBADF);
      continue;
    }
  }
}

static void unstash_file_descriptors(void) {
  int fd;

  for (fd = 0; fd < MAX_FILE_DESCRIPTOR; ++fd) {
    int alt_fd = fd + MAX_FILE_DESCRIPTOR;
    if (dup2(alt_fd, fd) < 0) {
      assert(errno == EBADF);
      continue;
    }
    close(alt_fd);
  }
}

NACL_SPAWN_TLS jmp_buf nacl_spawn_vfork_env;
static NACL_SPAWN_TLS pid_t vfork_pid = -1;
static NACL_SPAWN_TLS int vforking = 0;

// Shared spawnve implementation. Declared static so that shared library
// overrides doesn't break calls meant to be internal to this implementation.
static int spawnve_impl(int mode,
                        const char* path,
                        char* const argv[],
                        char* const envp[]) {
  if (NULL == path || NULL == argv[0]) {
    errno = EINVAL;
    return -1;
  }
  if (mode == P_WAIT) {
    int pid = spawnve_impl(P_NOWAIT, path, argv, envp);
    if (pid < 0) {
      return -1;
    }
    int status;
    int result = waitpid_impl(pid, &status, 0);
    if (result < 0) {
      return -1;
    }
    return status;
  } else if (mode == P_NOWAIT || mode == P_NOWAITO) {
    // The normal case.
  } else if (mode == P_OVERLAY) {
    if (vforking) {
      vfork_pid = spawnve_impl(P_NOWAIT, path, argv, envp);
      longjmp(nacl_spawn_vfork_env, 1);
    }
    // TODO(bradnelson): Add this by allowing javascript to replace the
    // existing module with a new one.
    errno = ENOSYS;
    return -1;
  } else {
    errno = EINVAL;
    return -1;
  }
  if (NULL == envp) {
    envp = environ;
  }

  struct PP_Var req_var = VarDictionaryCreate();
  VarDictionarySetString(req_var, "command", "nacl_spawn");

  struct PP_Var args_var = VarArrayCreate();
  for (int i = 0; argv[i]; i++)
    VarArraySetString(args_var, i, argv[i]);
  VarDictionarySet(req_var, "args", args_var);

  struct PP_Var envs_var = VarArrayCreate();
  for (int i = 0; envp[i]; i++)
    VarArraySetString(envs_var, i, envp[i]);

  if (CloneFileDescriptors(envs_var) < 0) {
    return -1;
  }

  VarDictionarySet(req_var, "envs", envs_var);
  VarDictionarySetString(req_var, "cwd", GetCwd().c_str());

  if (!AddNmfToRequest(path, req_var)) {
    errno = ENOENT;
    return -1;
  }

  return GetIntAndRelease(SendRequest(req_var), "pid");
}

// Spawn a new NaCl process. This is an alias for
// spawnve(mode, path, argv, NULL). Returns 0 on success. On error -1 is
// returned and errno will be set appropriately.
int spawnv(int mode, const char* path, char* const argv[]) {
  return spawnve_impl(mode, path, argv, NULL);
}

int spawnve(int mode, const char* path,
            char* const argv[], char* const envp[]) {
  return spawnve_impl(mode, path, argv, envp);
}

// Shared below by waitpid and wait.
// Done as a static so that users that replace waitpid and call wait (gcc)
// don't cause infinite recursion.
static pid_t waitpid_impl(int pid, int* status, int options) {
  struct PP_Var req_var = VarDictionaryCreate();
  VarDictionarySetString(req_var, "command", "nacl_wait");
  VarDictionarySet(req_var, "pid", PP_MakeInt32(pid));
  VarDictionarySet(req_var, "options", PP_MakeInt32(options));

  struct PP_Var result_var = SendRequest(req_var);
  int result_pid = GetInt(result_var, "pid");

  // WEXITSTATUS(s) is defined as ((s >> 8) & 0xff).
  struct PP_Var status_var;
  if (VarDictionaryHasKey(result_var, "status", &status_var)) {
    int raw_status = status_var.value.as_int;
    *status = (raw_status & 0xff) << 8;
  }
  VarRelease(result_var);
  return result_pid;
}

extern "C" {

#if defined(__GLIBC__)
pid_t wait(void* status) {
#else
pid_t wait(int* status) {
#endif
  return waitpid_impl(-1, static_cast<int*>(status), 0);
}

// Waits for the specified pid. The semantics of this function is as
// same as waitpid, though this implementation has some restrictions.
// Returns 0 on success. On error -1 is returned and errno will be set
// appropriately.
pid_t waitpid(pid_t pid, int* status, int options) {
  return waitpid_impl(pid, status, options);
}

// BSD wait variant with rusage.
#if defined(__BIONIC__)
pid_t wait3(int* status, int options, struct rusage* unused_rusage) {
#else
pid_t wait3(void* status, int options, struct rusage* unused_rusage) {
#endif
  return waitpid_impl(-1, static_cast<int*>(status), options);
}

// BSD wait variant with pid and rusage.
#if defined(__BIONIC__)
pid_t wait4(pid_t pid, int* status, int options,
            struct rusage* unused_rusage) {
#else
pid_t wait4(pid_t pid, void* status, int options,
            struct rusage* unused_rusage) {
#endif
  return waitpid_impl(pid, static_cast<int*>(status), options);
}

/*
 * Fake version of getpid().  This is used if there is no
 * nacl_spawn_ppid set and no IRT getpid interface available.
 */
static int getpid_fake(int* pid) {
  *pid = 1;
  return 0;
}

static struct nacl_irt_dev_getpid irt_dev_getpid;

/*
 * IRT version of getpid().  This is used if there is no
 * nacl_spawn_ppid set.
 */
static pid_t getpid_irt() {
  if (irt_dev_getpid.getpid == NULL) {
    int res = nacl_interface_query(NACL_IRT_DEV_GETPID_v0_1,
                                   &irt_dev_getpid,
                                   sizeof(irt_dev_getpid));
    if (res != sizeof(irt_dev_getpid)) {
      irt_dev_getpid.getpid = getpid_fake;
    }
  }

  int pid;
  int error = irt_dev_getpid.getpid(&pid);
  if (error != 0) {
    errno = error;
    return -1;
  }
  return pid;
}

// Get the process ID of the calling process.
pid_t getpid() {
  if (nacl_spawn_pid == -1) {
    return getpid_irt();
  }
  return nacl_spawn_pid;
}

// Get the process ID of the parent process.
pid_t getppid() {
  if (nacl_spawn_ppid == -1) {
    errno = ENOSYS;
  }
  return nacl_spawn_ppid;
}

// Spawn a process.
int posix_spawn(
    pid_t* pid, const char* path,
    const posix_spawn_file_actions_t* file_actions,
    const posix_spawnattr_t* attrp,
    char* const argv[], char* const envp[]) {
  int ret = spawnve_impl(P_NOWAIT, path, argv, envp);
  if (ret < 0) {
    return ret;
  }
  *pid = ret;
  return 0;
}

// Spawn a process using PATH to resolve.
int posix_spawnp(
    pid_t* pid, const char* file,
    const posix_spawn_file_actions_t* file_actions,
    const posix_spawnattr_t* attrp,
    char* const argv[], char* const envp[]) {
  // TODO(bradnelson): Make path expansion optional.
  return posix_spawn(pid, file, file_actions, attrp, argv, envp);
}

// Get the process group ID of the given process.
pid_t getpgid(pid_t pid) {
  struct PP_Var req_var = VarDictionaryCreate();
  VarDictionarySetString(req_var, "command", "nacl_getpgid");
  VarDictionarySet(req_var, "pid", PP_MakeInt32(pid));

  return GetIntAndRelease(SendRequest(req_var), "pgid");
}

// Get the process group ID of the current process. This is an alias for
// getpgid(0).
pid_t getpgrp() {
  return getpgid(0);
}

// Set the process group ID of the given process.
pid_t setpgid(pid_t pid, pid_t pgid) {
  struct PP_Var req_var = VarDictionaryCreate();
  VarDictionarySetString(req_var, "command", "nacl_setpgid");
  VarDictionarySet(req_var, "pid", PP_MakeInt32(pid));
  VarDictionarySet(req_var, "pgid", PP_MakeInt32(pgid));

  return GetIntAndRelease(SendRequest(req_var), "result");
}

// Set the process group ID of the given process. This is an alias for
// setpgid(0, 0).
pid_t setpgrp() {
  return setpgid(0, 0);
}

// Get the session ID of the given process.
pid_t getsid(pid_t pid) {
  struct PP_Var req_var = VarDictionaryCreate();
  VarDictionarySetString(req_var, "command", "nacl_getsid");
  VarDictionarySet(req_var, "pid", PP_MakeInt32(pid));
  return GetIntAndRelease(SendRequest(req_var), "sid");
}

// Make the current process a session leader.
pid_t setsid() {
  struct PP_Var req_var = VarDictionaryCreate();
  VarDictionarySetString(req_var, "command", "nacl_setsid");
  return GetIntAndRelease(SendRequest(req_var), "sid");
}

void jseval(const char* cmd, char** data, size_t* len) {
  struct PP_Var req_var = VarDictionaryCreate();
  VarDictionarySetString(req_var, "command", "nacl_jseval");
  VarDictionarySetString(req_var, "cmd", cmd);

  struct PP_Var result_dict_var = SendRequest(req_var);
  struct PP_Var result_var = VarDictionaryGet(result_dict_var, "result");
  uint32_t result_len;
  const char* result = PSInterfaceVar()->VarToUtf8(result_var, &result_len);
  if (len) {
    *len = result_len;
  }
  if (data) {
    *data = static_cast<char*>(malloc(result_len + 1));
    assert(*data);
    memcpy(*data, result, result_len);
    (*data)[result_len] = '\0';
  }
  VarRelease(result_var);
  VarRelease(result_dict_var);
}

static void mountfs() {
  struct PP_Var req_var = VarDictionaryCreate();
  VarDictionarySetString(req_var, "command", "nacl_mountfs");
  struct PP_Var result_dict_var = SendRequest(req_var);

  MountLocalFs(result_dict_var);
  VarRelease(result_dict_var);

  PSEventRegisterMessageHandler("mount", &HandleMountMessage, NULL);
  PSEventRegisterMessageHandler("unmount", &HandleUnmountMessage, NULL);
}

// Create a pipe. pipefd[0] will be the read end of the pipe and pipefd[1] the
// write end of the pipe.
int pipe(int pipefd[2]) {
  if (pipefd == NULL) {
    errno = EFAULT;
    return -1;
  }

  struct PP_Var req_var = VarDictionaryCreate();
  VarDictionarySetString(req_var, "command", "nacl_apipe");

  struct PP_Var result_var = SendRequest(req_var);
  int id = GetInt(result_var, "pipe_id");
  VarRelease(result_var);

  int read_fd;
  int write_fd;
  char path[100];
  sprintf(path, "/apipe/%d", id);
  read_fd = open(path, O_RDONLY);
  write_fd = open(path, O_WRONLY);
  if (read_fd < 0 || write_fd < 0) {
    if (read_fd >= 0) {
      close(read_fd);
    }
    if (write_fd >= 0) {
      close(write_fd);
    }
    return -1;
  }
  pipefd[0] = read_fd;
  pipefd[1] = write_fd;

  return 0;
}

void nacl_spawn_vfork_before(void) {
  assert(!vforking);
  vforking = 1;
  stash_file_descriptors();
}

pid_t nacl_spawn_vfork_after(int jmping) {
  if (jmping) {
    unstash_file_descriptors();
    vforking = 0;
    return vfork_pid;
  }
  return 0;
}

void nacl_spawn_vfork_exit(int status) {
  if (vforking) {
    struct PP_Var req_var = VarDictionaryCreate();
    VarDictionarySetString(req_var, "command", "nacl_deadpid");
    VarDictionarySet(req_var, "status", PP_MakeInt32(status));

    int result = GetIntAndRelease(SendRequest(req_var), "pid");
    if (result < 0) {
      errno = -result;
      vfork_pid = -1;
    } else {
      vfork_pid = result;
    }
    longjmp(nacl_spawn_vfork_env, 1);
  } else {
    _exit(status);
  }
}

void nacl_setup_env() {
  // If we running in sel_ldr then don't do any the filesystem/nacl_io
  // setup. We detect sel_ldr by the absence of the Pepper Instance.
  if (PSGetInstanceId() == 0) {
    return;
  }

  umount("/");
  do_mount("", "/", "memfs", 0, NULL);

  setup_anonymous_pipes();

  // Setup common environment variables, but don't override those
  // set already by ppapi_simple.
  setenv("HOME", "/home/user", 0);
  setenv("PATH", "/bin", 0);
  setenv("USER", "user", 0);
  setenv("LOGNAME", "user", 0);

  const char* home = getenv("HOME");
  mkdir_checked("/home");
  mkdir_checked(home);
  mkdir_checked("/tmp");
  mkdir_checked("/bin");
  mkdir_checked("/etc");
  mkdir_checked("/mnt");
  mkdir_checked("/mnt/http");
  mkdir_checked("/mnt/html5");

  const char* data_url = getenv("NACL_DATA_URL");
  if (!data_url)
    data_url = "./";
  NACL_LOG("NACL_DATA_URL=%s\n", data_url);

  const char* mount_flags = getenv("NACL_DATA_MOUNT_FLAGS");
  if (!mount_flags)
    mount_flags = "";
  NACL_LOG("NACL_DATA_MOUNT_FLAGS=%s\n", mount_flags);

  if (do_mount(data_url, "/mnt/http", "httpfs", 0, mount_flags) != 0) {
    perror("mounting http filesystem at /mnt/http failed");
  }

  if (do_mount("/", "/mnt/html5", "html5fs", 0, "type=PERSISTENT") != 0) {
    perror("Mounting HTML5 filesystem in /mnt/html5 failed");
  } else {
    mkdir("/mnt/html5/home", 0777);
    struct stat st;
    if (stat("/mnt/html5/home", &st) < 0 || !S_ISDIR(st.st_mode)) {
      perror("Unable to create home directory in persistent storage");
    } else {
      if (do_mount("/home", home, "html5fs", 0, "type=PERSISTENT") != 0) {
        fprintf(stderr, "Mounting HTML5 filesystem in %s failed.\n", home);
      }
    }
  }

  if (do_mount("/", "/tmp", "html5fs", 0, "type=TEMPORARY") != 0) {
    perror("Mounting HTML5 filesystem in /tmp failed");
  }

  mountfs();

  /* naclprocess.js sends the current working directory using this
   * environment variable. */
  const char* pwd = getenv("PWD");
  if (pwd != NULL) {
    if (chdir(pwd)) {
      fprintf(stderr, "chdir() to %s failed: %s\n", pwd, strerror(errno));
    }
  }

  // Tell the NaCl architecture to /etc/bashrc of mingn.
#if defined(__x86_64__)
  static const char kNaClArch[] = "x86_64";
  // Use __i386__ rather then __i686__ since the latter is not defined
  // by i686-nacl-clang.
#elif defined(__i386__)
  static const char kNaClArch[] = "i686";
#elif defined(__arm__)
  static const char kNaClArch[] = "arm";
#elif defined(__pnacl__)
  static const char kNaClArch[] = "pnacl";
#else
# error "Unknown architecture"
#endif
  // Set NACL_ARCH with a guess if not set (0 == set if not already).
  setenv("NACL_ARCH", kNaClArch, 0);
  // Set NACL_BOOT_ARCH if not inherited from a parent (0 == set if not already
  // set). This will let us prefer PNaCl if we started with PNaCl (for tests
  // mainly).
  setenv("NACL_BOOT_ARCH", kNaClArch, 0);

  setlocale(LC_CTYPE, "");

  nacl_spawn_pid = getenv_as_int("NACL_PID");
  nacl_spawn_ppid = getenv_as_int("NACL_PPID");

  restore_pipes();
}

#define VARG_TO_ARGV_START \
  va_list vl; \
  va_start(vl, arg); \
  va_list vl_count; \
  va_copy(vl_count, vl); \
  int count = 1; \
  while (va_arg(vl_count, char*)) { \
    ++count; \
  } \
  va_end(vl_count); \
  /* Copying all the args into argv plus a trailing NULL */ \
  char** argv = static_cast<char**>(alloca(sizeof(char *) * (count + 1))); \
  argv[0] = const_cast<char*>(arg); \
  for (int i = 1; i <= count; i++) { \
    argv[i] = va_arg(vl, char*); \
  }

#define VARG_TO_ARGV \
  VARG_TO_ARGV_START; \
  va_end(vl);

#define VARG_TO_ARGV_ENVP \
  VARG_TO_ARGV_START; \
  char* const* envp = va_arg(vl, char* const*); \
  va_end(vl);

int execve(const char *filename, char *const argv[], char *const envp[]) {
  return spawnve_impl(P_OVERLAY, filename, argv, envp);
}

int execv(const char *path, char *const argv[]) {
  return spawnve_impl(P_OVERLAY, path, argv, environ);
}

int execvp(const char *file, char *const argv[]) {
  // TODO(bradnelson): Limit path resolution to 'p' variants.
  return spawnve_impl(P_OVERLAY, file, argv, environ);
}

int execvpe(const char *file, char *const argv[], char *const envp[]) {
  // TODO(bradnelson): Limit path resolution to 'p' variants.
  return spawnve_impl(P_OVERLAY, file, argv, envp);
}

int execl(const char *path, const char *arg, ...) {
  VARG_TO_ARGV;
  return spawnve_impl(P_OVERLAY, path, argv, environ);
}

int execlp(const char *file, const char *arg, ...) {
  VARG_TO_ARGV;
  // TODO(bradnelson): Limit path resolution to 'p' variants.
  return spawnve_impl(P_OVERLAY, file, argv, environ);
}

int execle(const char *path, const char *arg, ...) {  /* char* const envp[] */
  VARG_TO_ARGV_ENVP;
  return spawnve_impl(P_OVERLAY, path, argv, envp);
}

int execlpe(const char *path, const char *arg, ...) {  /* char* const envp[] */
  VARG_TO_ARGV_ENVP;
  // TODO(bradnelson): Limit path resolution to 'p' variants.
  return spawnve_impl(P_OVERLAY, path, argv, envp);
}

};  // extern "C"
