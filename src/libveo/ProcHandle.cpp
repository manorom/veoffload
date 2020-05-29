/**
 * @file ProcHandle.cpp
 * @brief implementation of ProcHandle
 */
#include "ProcHandle.hpp"
#include "ThreadContext.hpp"
#include "VEOException.hpp"
#include "CallArgs.hpp"
#include "log.hpp"
#include "CommandImpl.hpp"

#include <string.h>
#include <unistd.h>
#include <sys/auxv.h>
#include <sys/ipc.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/shm.h>
#include <libgen.h>

#include <libved.h>
/* VE OS internal headers */
extern "C" {
#define new new__ // avoid keyword
#include "comm_request.h"
#include "handle.h"
#include "mm_type.h"
#include "process_mgmt_comm.h"
#include "sys_process_mgmt.h"
#include "vemva_mgmt.h"
#include "loader.h"
#include "pseudo_ptrace.h"

void ve_sa_sigaction_handler(int, siginfo_t *, void *);
#undef new

/* symbols required but undefined in libvepseudo */
extern __thread veos_handle *g_handle;
extern struct tid_info global_tid_info[VEOS_MAX_VE_THREADS];
extern __thread sigset_t ve_proc_sigmask;


// copied from pseudo_process.c
/**
 * @brief Fetches the node number from VEOS socket file.
 *
 * @param[in] s string contains the veos socket file path
 *
 * @return node number on which this ve process will run
 */
static int get_ve_node_num(char *s)
{
        int n = 0;

        while (*s != '\0') {
                if (std::isdigit(*s))
                        n = 10*n + (*s - '0');
                else if (*s == '.')
                        break;

                s++;
        }
        return n;
}

// copied from pseudo_process.c
/**
 * @brief Create shared memory region used for system call arguments.
 *
 * @param[in] handle VE OS handle
 * @param[out] node_id Provide the node_id on which this ve process will run
 * @param[out] sfile_name randomly generated file name with complete path
 * @return file descriptor of shared memory used for LHM-SHM area 
 *         upon success; -1 upon failure.
 */
int init_lhm_shm_area(veos_handle *handle, int *node_id, char* sfile_name)
{
  using veo::VEO_LOG_ERROR;
  using veo::VEO_LOG_DEBUG;
  using veo::VEO_LOG_TRACE;
  int retval = -1, fd = -1; 
  char *base_name = nullptr, *dir_name = nullptr; 
  uint64_t shm_lhm_area = 0;
  veo::ThreadContext *ctx = nullptr;
  VEO_TRACE(ctx, "Entering %s", __func__);

  std::unique_ptr<char[]> tmp_sock0(new char[NAME_MAX+PATH_MAX]);
  std::unique_ptr<char[]> tmp_sock1(new char[NAME_MAX+PATH_MAX]);
  std::unique_ptr<char[]> shared_tmp_file(new char[NAME_MAX+PATH_MAX]);

  strncpy(tmp_sock0.get(), handle->veos_sock_name, NAME_MAX+PATH_MAX);
  strncpy(tmp_sock1.get(), handle->veos_sock_name, NAME_MAX+PATH_MAX);
  base_name = basename(tmp_sock0.get());
  dir_name = dirname(tmp_sock1.get());

  /* get node number from veos socket file basename */
  *node_id = get_ve_node_num(base_name);
  sprintf(shared_tmp_file.get(), "%s/veos%d-tmp/ve_exec_XXXXXX", dir_name, *node_id);

  VEO_DEBUG(ctx, "Shared file path: %s", shared_tmp_file.get());

  /* create a unique temporary file and opens it */
  fd = mkstemp(shared_tmp_file.get());
  if (fd < 0) {
    VEO_DEBUG(ctx, "mkstemp fails: %s", strerror(errno));
    goto hndl_return;
  }

  /* truncate file to size PAGE_SIZE_4KB */
  retval = ftruncate(fd, PAGE_SIZE_4KB);
  if (-1 == retval) {
    VEO_DEBUG(ctx, "ftruncate fails: %s", strerror(errno));
    goto hndl_return;
  }

  /* map the file in shared mode */
  shm_lhm_area = (uint64_t)mmap(NULL, PAGE_SIZE_4KB, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
  if ((void *)-1 == (void *)(shm_lhm_area)) {
    VEO_DEBUG(ctx, "Failed to map file, return value %s", strerror(errno));
    retval = -1;
    goto hndl_return;
  }

  VEO_DEBUG(ctx, "%lx", shm_lhm_area);
  memset((void *)shm_lhm_area, 0, PAGE_SIZE_4KB);
  vedl_set_shm_lhm_addr(handle->ve_handle, (void *)shm_lhm_area);
  strncpy(sfile_name, shared_tmp_file.get(), NAME_MAX+PATH_MAX);
  VEO_DEBUG(ctx, "Unique syscall args filename: %s", sfile_name);
  retval = fd;

hndl_return:
  PSEUDO_TRACE("Exiting");
  return retval;
}

/**
 * @brief abort pseudo process
 *
 * Functions in libvepseudo call pseudo_abort() on fatal error.
 */
void pseudo_abort()
{
  abort();
}
/* end of symbols required */
} // extern "C"

#ifndef PAGE_SIZE_4KB
#define PAGE_SIZE_4KB (4 * 1024)
#endif

namespace veo {
namespace internal {
  std::mutex spawn_mtx;
  unsigned int proc_no;
}

/**
 * @brief Initializes rwlock on DMA and fork
 *
 * This function initiazes a lock which will be used to synchronize
 * request related to DMA(reading data from VE memory) and creating new
 * process(fork/vfork). Execution of both requests parallely leads to
 * memory curruption.
 *
 * @return abort on failure.
 */
void init_rwlock_to_sync_dma_fork()
{
  // copied from pseudo_process.c
  int ret = -1;
  pthread_rwlockattr_t sync_fork_dma_attr;

  ret = pthread_rwlockattr_init(&sync_fork_dma_attr);
  if (ret) {
    PSEUDO_ERROR("Failed to initialize attribute %s", strerror(ret));
    fprintf(stderr, "VE process setup failed\n");
    pseudo_abort();
  }

  ret = pthread_rwlockattr_setkind_np(&sync_fork_dma_attr,
          PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP);
  if (ret) {
    PSEUDO_ERROR("Failed to set rwlock attribute: %s", strerror(ret));
    fprintf(stderr, "VE process setup failed\n");
    pseudo_abort();
  }

  ret = pthread_rwlock_init(&sync_fork_dma, &sync_fork_dma_attr);
  if (ret) {
    PSEUDO_ERROR("Failed to init rwlock %s", strerror(ret));
    fprintf(stderr, "VE process setup failed\n");
    pseudo_abort();
  }

  ret = pthread_rwlockattr_destroy(&sync_fork_dma_attr);
  if (ret) {
    PSEUDO_ERROR("Failed to destroy rwlock attribute: %s", strerror(ret));
  }
}

/**
 * @brief close the fd of syscall args file and remove the file.
 *
 * @param[in] fd, contains the fd of syscall args file.
 * @param[in] sfile_name string contains the syscall args file path.
 */
void close_syscall_args_file(int fd, char *sfile_name)
{
        PSEUDO_TRACE("Entering");

        close(fd);
        unlink(sfile_name);
        // free(sfile_name);

        PSEUDO_TRACE("Exiting");
}

void veo_sigcont_handler(int signo, siginfo_t *siginfo, void *uctx)
{
  VEO_ASSERT(signo == SIGCONT);
  if (g_handle) {
    ve_sa_sigaction_handler(signo, siginfo, uctx);
  } else {
    // This thread cannot handle the signal because it does not have
    // its VEOS handle. Send the same signal again.
    kill(getpid(), signo);
    sched_yield();
  }
}

/**
 * @brief create a VE process and initialize a thread context
 *
 * @param[in,out] ctx the thread context of the main thread
 * @param oshandle VE OS handle for the VE process (main thread)
 * @param binname VE executable
 */
int spawn_helper(ThreadContext *ctx, veos_handle *oshandle, const char *binname)
{
  /* necessary to allocate PATH_MAX because VE OS requests to
   * transfer PATH_MAX. */
  char helper_name[PATH_MAX];
  strncpy(helper_name, binname, sizeof(helper_name));
  int core_id, node_id, numa_node;
  char *file_name = nullptr;
  int ret = -1;
  std::unique_ptr<char[]> sfile_name(new char[NAME_MAX+PATH_MAX]);

  // libvepseudo touches PTRACE_PRIVATE_DATA area.
  void *ptrace_private = mmap((void *)PTRACE_PRIVATE_DATA, 4096,
                              PROT_READ|PROT_WRITE,
                              MAP_ANON|MAP_PRIVATE|MAP_FIXED, -1, 0);
  int saved_errno = errno;
  if (MAP_FAILED == ptrace_private) {
    VEO_DEBUG(ctx, "Fail to alloc chunk for ptrace private: %s", strerror(errno));
    throw VEOException("Failled to allocate ptrace related data", saved_errno);
  }

  /* Check if the request address is obtained or not */
  if (ptrace_private != (void *)PTRACE_PRIVATE_DATA) {
    VEO_DEBUG(ctx, "Request: %lx but got: %p for ptrace data.", PTRACE_PRIVATE_DATA, ptrace_private);
    munmap(ptrace_private, 4096);
    throw VEOException("Failled to allocate ptrace related data", saved_errno);
  }

  memset(ptrace_private, 0, 4096);

  // Set global TID array for main thread.
  global_tid_info[0].vefd = oshandle->ve_handle->vefd;
  global_tid_info[0].veos_hndl = oshandle;
  pthread_mutex_lock(&tid_counter_mutex);
  tid_counter = 0;
  pthread_mutex_unlock(&tid_counter_mutex);
  global_tid_info[0].tid_val = syscall(SYS_gettid); /*getpid();*/ // main thread
  global_tid_info[0].flag = 0;
  global_tid_info[0].mutex = PTHREAD_MUTEX_INITIALIZER;
  global_tid_info[0].cond = PTHREAD_COND_INITIALIZER;
  init_rwlock_to_sync_dma_fork();

  // Initialize the syscall argument area.
  ret = init_lhm_shm_area(oshandle, &node_id, sfile_name.get());
  if (ret < 0) {
    throw VEOException("failed to create temporary file.", 0);
  }

  // Request VE OS to create a new VE process
  new_ve_proc ve_proc = {0};
  // TODO: set resource limit.
  memset(&ve_proc.lim, -1, sizeof(ve_proc.lim));
  ve_proc.namespace_pid = syscall(SYS_gettid);
  ve_proc.shm_lhm_addr = (uint64_t)vedl_get_shm_lhm_addr(oshandle->ve_handle);
  ve_proc.core_id = -1;
  ve_proc.node_id = node_id;
  ve_proc.traced_proc = 0;
  ve_proc.tracer_pid = getppid();
  ve_proc.exec_path = (uint64_t)helper_name;
  ve_proc.numa_node = -1;
  file_name = basename(sfile_name.get());
  auto exe_name_buf = strdup(binname);
  auto exe_base_name = basename(exe_name_buf);
  memset(ve_proc.exe_name, '\0', ACCT_COMM);
  strncpy(ve_proc.exe_name, exe_base_name, ACCT_COMM - 1);
  strncpy(ve_proc.sfile_name, file_name, S_FILE_LEN - 1);
  free(exe_name_buf);

  int retval = pseudo_psm_send_new_ve_process(oshandle->veos_sock_fd, ve_proc);
  if (0 > retval) {
    close_syscall_args_file(ret, sfile_name.get());
    VEO_ERROR(ctx, "Failed to send NEW VE PROC request (%d)", retval);
    return retval;
  }
  retval = pseudo_psm_recv_load_binary_req(oshandle->veos_sock_fd,
                                           &core_id, &node_id, &numa_node);
  VEO_DEBUG(ctx, "CORE ID : %d\t NODE ID : %d NUMA NODE ID : %d", core_id, node_id, numa_node);
  if (0 > retval) {
    close_syscall_args_file(ret, sfile_name.get());
    VEO_ERROR(ctx, "VEOS acknowledgement error (%d)", retval);
    return retval;
  }

  /* close the fd of syscall args file and remove the file */
  close_syscall_args_file(ret, sfile_name.get());

  vedl_set_syscall_area_offset(oshandle->ve_handle, 0);

  // initialize VEMVA space
  INIT_LIST_HEAD(&vemva_header.vemva_list);
  retval = init_vemva_header();
  if (retval) {
    VEO_ERROR(ctx, "failed to initialize (%d)", retval);
    return retval;
  }

  // Load an executable
  struct ve_start_ve_req_cmd start_ve_req = {{0}};
  retval = pse_load_binary(helper_name, oshandle, &start_ve_req);
  if (retval) {
    VEO_ERROR(ctx, "failed to load ve binary (%d)", retval);
    process_thread_cleanup(oshandle, -1);
    return retval;
  }

  char *ve_argv[] = { helper_name, nullptr};

  // Allocate area for environment variables, NULL, auxiliary vectors and NULL
  int env_num = 0;
  for (char **envp = environ; NULL != *envp; envp++) {
    env_num++;
  }
  env_num += 1 + 2 * (32 - 1) + 1;
  char *env_array[env_num];

  // Copy environment variables and auxiliary vectors
  int env_index = 0;
  for (char **envp = environ; NULL != *envp; envp++) {
    env_array[env_index++] = *envp;
  }
  env_array[env_index++] = NULL;
  unsigned long auxv_type, auxv_val;
  for (auxv_type = 1; auxv_type < 32; auxv_type++) {
    switch (auxv_type) {
    // List of auxv that will be placed in VE process stack
    case AT_EXECFD:
    case AT_PHDR:
    case AT_PHENT:
    case AT_PHNUM:
    case AT_PAGESZ:
    case AT_BASE:
    case AT_FLAGS:
    case AT_ENTRY:
    case AT_UID:
    case AT_EUID:
    case AT_GID:
    case AT_EGID:
    case AT_PLATFORM:
    case AT_CLKTCK:
    case AT_SECURE:
    case AT_RANDOM:
    case AT_EXECFN:
    case AT_QUICKCALL_VADDR:
      auxv_val = getauxval(auxv_type);
      if (auxv_val) {
        env_array[env_index++] = (char *)auxv_type;
        env_array[env_index++] = (char *)auxv_val;
      }
      break;
    default:
      break;
    }
  }
  env_array[env_index++] = NULL;

  // initialize the stack
  retval = init_stack(oshandle, 1, ve_argv, env_array, &start_ve_req);
  if (retval) {
    VEO_ERROR(ctx, "failed to make stack region (%d)", retval);
    process_thread_cleanup(oshandle, -1);
    return retval;
  }
  memcpy(&start_ve_req.ve_info, &ve_info,
    sizeof(struct ve_address_space_info_cmd));

  // start VE process
  retval = pseudo_psm_send_start_ve_proc_req(&start_ve_req,
             oshandle->veos_sock_fd);
  if (0 > retval) {
    VEO_ERROR(ctx, "failed to send start VE process request (%d)", retval);
    return retval;
  }
  retval = pseudo_psm_recv_start_ve_proc(oshandle->veos_sock_fd);
  if (0 > retval) {
    VEO_ERROR(ctx, "Failed to receive START VE PROC ack (%d)", retval);
    return retval;
  }
  // register a signal handler
  struct sigaction pseudo_act;
  memset(&pseudo_act, 0, sizeof(pseudo_act));
  pseudo_act.sa_sigaction = &veo_sigcont_handler;
  pseudo_act.sa_flags = SA_SIGINFO;
  retval = sigaction(SIGCONT, &pseudo_act, NULL);
  if (0 > retval) {
    VEO_ERROR(ctx, "sigaction for SIGCONT failed (errno = %d)", errno);
    process_thread_cleanup(oshandle, -1);
    return retval;
  }
  VEO_TRACE(ctx, "%s: Succeed to create a VE process.", __func__);
  return 0;
}

/**
 * @brief constructor
 *
 * @param ossock path to VE OS socket
 * @param vedev path to VE device file
 * @param binname VE executable
 */
ProcHandle::ProcHandle(const char *ossock, const char *vedev,
                       const char *binname)
{
  int retval;
  size_t funcs_sz;

  class Finally {
    sigset_t saved_mask;
  public:
    Finally() {
      sigset_t mask;
      sigfillset(&mask);
      sigprocmask(SIG_BLOCK, &mask, &this->saved_mask);
      memcpy(&ve_proc_sigmask, &this->saved_mask, sizeof(this->saved_mask));
    }
    ~Finally() {
      // restore the signal mask of main thread on return
      sigprocmask(SIG_SETMASK, &this->saved_mask, nullptr);
      // Thread-local VEOS handle for main is no longer used.
      g_handle = NULL;
    }
  } finally_;

  // determine VE#
  int nmatch = sscanf(vedev, "/dev/veslot%d", &this->ve_number);
  if (nmatch != 1) {
    VEO_DEBUG(nullptr, "cannot determine VE node#: %s", vedev);
    this->ve_number = -1;
  }

  // open VE OS handle
  veos_handle *os_handle = veos_handle_create(const_cast<char *>(vedev),
                             const_cast<char *>(ossock), nullptr, -1);
  if (os_handle == NULL) {
    throw VEOException("veos_handle_create failed.");
  }
  g_handle = os_handle;
  // initialize the main thread context
  this->main_thread.reset(new ThreadContext(this, os_handle, true));

  std::lock_guard<std::mutex> lock(internal::spawn_mtx);
  if (internal::proc_no != 0) {
    veos_handle_free(os_handle);
    throw VEOException("The creation of a VE process failed.", 0);
  }
  if (spawn_helper(this->main_thread.get(), os_handle, binname) != 0) {
    veos_handle_free(os_handle);
    throw VEOException("The creation of a VE process failed.", 0);
  }
  ++internal::proc_no;  

  // VE process is ready here. The state is changed to RUNNING.
  this->main_thread->state = VEO_STATE_RUNNING;

  // handle some system calls from main thread for initialization of VE libc.
  this->waitForBlock();
  // VE process is to stop at the first block here.
  // sysve(VEO_BLOCK, &veo__helper_functions);
  uint64_t funcs_addr = this->main_thread->_collectReturnValue();
  VEO_DEBUG(this->main_thread.get(), "helper functions set: %p\n", (void *)funcs_addr);
  int rv = ve_recv_data(os_handle, funcs_addr, sizeof(uint64_t), &this->funcs);
  if (rv != 0) {
    throw VEOException("Failed to receive data from VE");
  }
  VEO_ASSERT(this->funcs.version >= VEORUN_VERSION2);
  if (this->funcs.version == VEORUN_VERSION2) {
    funcs_sz =  sizeof(struct veo__helper_functions_ver2);
  } else if (this->funcs.version == VEORUN_VERSION3) {
    funcs_sz = sizeof(struct veo__helper_functions_ver3);
  } else if (this->funcs.version == VEORUN_VERSION4) {
    funcs_sz = sizeof(struct veo__helper_functions_ver4);
  } else {
    throw VEOException("Invalid VEORUN_VERSION");
  }
  rv = ve_recv_data(os_handle, funcs_addr, funcs_sz, &this->funcs);
  if (rv != 0) {
    throw VEOException("Failed to receive data from VE");
  }
#define DEBUG_PRINT_HELPER(ctx, data, member) \
  VEO_DEBUG(ctx, #member " = %#lx", data.member)
  DEBUG_PRINT_HELPER(this->main_thread.get(), this->funcs, version);
  DEBUG_PRINT_HELPER(this->main_thread.get(), this->funcs, load_library);
  DEBUG_PRINT_HELPER(this->main_thread.get(), this->funcs, alloc_buff);
  DEBUG_PRINT_HELPER(this->main_thread.get(), this->funcs, free_buff);
  DEBUG_PRINT_HELPER(this->main_thread.get(), this->funcs, find_sym);
  DEBUG_PRINT_HELPER(this->main_thread.get(), this->funcs, create_thread);
  DEBUG_PRINT_HELPER(this->main_thread.get(), this->funcs, call_func);
  DEBUG_PRINT_HELPER(this->main_thread.get(), this->funcs, exit);
  if (this->funcs.version >= VEORUN_VERSION3)
    DEBUG_PRINT_HELPER(this->main_thread.get(), this->funcs,
                       create_thread_with_attr);
  if (this->funcs.version >= VEORUN_VERSION4)
    DEBUG_PRINT_HELPER(this->main_thread.get(), this->funcs,
		       load_library_err);
  // create worker
  CallArgs args_create_thread;
  args_create_thread.set(0, -1);// FIXME: get the number of cores on VE
  this->main_thread->_doCall(this->funcs.create_thread, args_create_thread);
  uint64_t exc;
  // hook clone() on VE
  auto req = this->main_thread->exceptionHandler(exc,
               &ThreadContext::hookCloneFilter);
  if (!_is_clone_request(req)) {
    throw VEOException("VE process requests block unexpectedly.", 0);
  }
  // create a new ThreadContext for a worker thread
  this->worker.reset(new ThreadContext(this, this->osHandle()));
  // handle clone() request.
  auto tid = this->worker->handleCloneRequest();
  if ( tid < 0 ) {
    VEO_ERROR(this->worker.get(), "worker->handleCloneRequest() failed. (errno = %d)", -tid);
  }
  // restart execution; execute until the next block request.
  this->main_thread->_unBlock(tid);
  this->waitForBlock();

  VEO_TRACE(this->worker.get(), "sp = %#lx", this->worker->ve_sp);
  pthread_mutex_lock(&tid_counter_mutex);
  this->setnumChildThreads(tid_counter);
  pthread_mutex_unlock(&tid_counter_mutex);
  VEO_DEBUG(this->worker.get(), "num_child_threads = %d", this->getnumChildThreads());
}

uint64_t doOnContext(ThreadContext *ctx, uint64_t func, CallArgs &args)
{
  VEO_TRACE(nullptr, "doOnContext(%p, %#lx, ...)", ctx, func);
  auto reqid = ctx->callAsync(func, args);
  uint64_t ret;
  int rv = ctx->callWaitResult(reqid, &ret);
  if (rv != VEO_COMMAND_OK) {
    VEO_ERROR(ctx, "function %#lx failed (%d)", func, rv);
    throw VEOException("request failed", ENOSYS);
  }
  return ret;
}

/**
 * @brief Set a num of child threads to check dynamic load or static link.
 * 
 * @param num child thread number when worker thread created
 */
int ProcHandle::setnumChildThreads(int num){
  num_child_threads = num;
}

/**
 * @brief Load a VE library in VE process space
 *
 * @param libname a library name
 * @return handle of the library loaded upon success; zero upon failure.
 */
uint64_t ProcHandle::loadLibrary(const char *libname)
{
  VEO_TRACE(this->worker.get(), "%s(%s)", __func__, libname);
  size_t len = strlen(libname);
  if (len > VEO_SYMNAME_LEN_MAX) {
    throw VEOException("Too long name", ENAMETOOLONG);
  }
  CallArgs args;// no argument.
  args.setOnStack(VEO_INTENT_IN, 0, const_cast<char *>(libname), len + 1);

  uint64_t handle = doOnContext(this->worker.get(),
                                this->funcs.load_library, args);
  VEO_TRACE(this->worker.get(), "handle = %#lx", handle);
  if ((handle == 0) && (this->getVeorunVersion() >= VEORUN_VERSION4)) {
    char err_msg[ERR_MSG_LEN] = {'\0'};
      if (this->loadLibraryError(err_msg, ERR_MSG_LEN) == 0)
        VEO_ERROR(this->worker.get(), "%s : %s", __func__, err_msg);
  }
  return handle;
}

/**
 * @brief Find a symbol in VE program
 *
 * @param libhdl handle of library
 * @param symname a symbol name to find
 * @return VEMVA of the symbol upon success; zero upon failure.
 */
uint64_t ProcHandle::getSym(const uint64_t libhdl, const char *symname)
{
  size_t len = strlen(symname);
  if (len > VEO_SYMNAME_LEN_MAX) {
    throw VEOException("Too long name", ENAMETOOLONG);
  }
  CallArgs args;
  args.set(0, libhdl);
  args.setOnStack(VEO_INTENT_IN, 1, const_cast<char *>(symname), len + 1);
  sym_mtx.lock();
  auto sym_pair = std::make_pair(libhdl, symname);
  auto itr = sym_name.find(sym_pair);
  if( itr != sym_name.end() ) {
    sym_mtx.unlock();
    VEO_TRACE(this->worker.get(), "symbol addr = %#lx", itr->second);
    VEO_TRACE(this->worker.get(), "symbol name = %s", symname);
    return itr->second;
  }
  sym_mtx.unlock();
  uint64_t symaddr = doOnContext(this->worker.get(),
                                 this->funcs.find_sym, args);
  VEO_TRACE(this->worker.get(), "symbol addr = %#lx", symaddr);
  VEO_TRACE(this->worker.get(), "symbol name = %s", symname);
  if (symaddr == NULL) {
    return symaddr;
  }
  sym_mtx.lock();
  sym_name[sym_pair] = symaddr;
  sym_mtx.unlock();
  return symaddr;
}

/**
 * @brief Allocate a buffer on VE
 *
 * @param size of buffer
 * @return VEMVA of the buffer upon success; zero upon failure.
 */
uint64_t ProcHandle::allocBuff(const size_t size)
{
  std::lock_guard<std::mutex> lock(this->main_mutex);
  CallArgs args{size};
  return doOnContext(this->worker.get(), this->funcs.alloc_buff, args);
}

/**
 * @brief Free a buffer on VE
 *
 * @param buff VEMVA of the buffer
 * @return nothing
 */
void ProcHandle::freeBuff(const uint64_t buff)
{
  std::lock_guard<std::mutex> lock(this->main_mutex);
  CallArgs args{buff};
  doOnContext(this->worker.get(), this->funcs.free_buff, args);
}

/**
 * @brief Exit veorun on VE side
 *
 */
void ProcHandle::exitProc()
{
  CallArgs args;
  uint64_t ret;
  uint64_t exc;

  std::lock_guard<std::mutex> lock(this->main_mutex);

  VEO_TRACE(nullptr, "call exit(%p, %#lx, ...)", this->worker.get(), this->funcs.exit);
  if ( this->funcs.exit == 0 || this->main_thread->state == VEO_STATE_EXIT)
    return;

  auto id = this->worker.get()->issueRequestID();
  auto f = [&args, this, id] (Command *cmd) {
    this->worker.get()->_doCall(this->funcs.exit, args);
    int status;
    uint64_t exs;
    auto successful = this->worker.get()->exceptionHandler(exs,
			&ThreadContext::exitFilter);
    if (!successful) {
      if (status == VEO_HANDLER_STATUS_EXCEPTION) {
        cmd->setResult(exs, VEO_COMMAND_EXCEPTION);
      } else {
        cmd->setResult(status, VEO_COMMAND_ERROR);
      }
      return 1;
    }
    cmd->setResult(0, VEO_COMMAND_OK);

    // post
    auto readmem = std::bind(&ThreadContext::_readMem, this->worker.get(),
                             std::placeholders::_1, std::placeholders::_2,
                             std::placeholders::_3);
    args.copyout(readmem);
    return 0;
  };
  std::unique_ptr<Command> req(new internal::CommandImpl(id, f));
  if(this->worker.get()->comq.pushRequest(std::move(req)))
    id = VEO_REQUEST_ID_INVALID;

  VEO_TRACE(nullptr, "[request #%d] pushRequest", id);
  auto c = this->worker.get()->comq.waitCompletion(id);

  /* exitProc() */
  VEO_TRACE(this->main_thread.get(), "%s()", __func__);
  process_thread_cleanup(this->osHandle(), -1);
  this->main_thread.get()->state = VEO_STATE_EXIT;
  veos_handle_free(this->osHandle());
  return;
}

/**
 * @brief open a new context (VE thread)
 *
 * @return a new thread context created
 */
ThreadContext *ProcHandle::openContext()
{
  CallArgs args;
  std::lock_guard<std::mutex> lock(this->main_mutex);

  auto ctx = this->worker.get();
  pthread_mutex_lock(&tid_counter_mutex);
  /* FIXME */
  int max_cpu_num = 8;
  if (tid_counter > max_cpu_num - 1) { /* FIXME */
    args.set(0, getnumChildThreads()%max_cpu_num);// same as worker thread
    VEO_DEBUG(ctx, "num_child_threads = %d", getnumChildThreads());
  } else {
    args.set(0, -1);// any cpu
    VEO_DEBUG(ctx, "num_child_threads = %d", getnumChildThreads());
  }
  pthread_mutex_unlock(&tid_counter_mutex);
  auto reqid = ctx->_callOpenContext(this, this->funcs.create_thread, args);
  uintptr_t ret;
  int rv = ctx->callWaitResult(reqid, &ret);
  if (rv != VEO_COMMAND_OK) {
    VEO_ERROR(ctx, "openContext failed (%d)", rv);
    throw VEOException("request failed", ENOSYS);
  }
  return reinterpret_cast<ThreadContext *>(ret);
}

/**
 * @brief open a new context (VE thread) with attributes
 *
 * @param[in] attr attributes of a new context
 *
 * @return a new thread context created
 */
ThreadContext *ProcHandle::openContext(ThreadContextAttr &attr)
{
  CallArgs args;
  std::lock_guard<std::mutex> lock(this->main_mutex);

  auto ctx = this->worker.get();
  pthread_mutex_lock(&tid_counter_mutex);
  /* FIXME */
  int max_cpu_num = 8;
  int cpu;
  struct veo__thread_attribute_ver3 attr_v3;
  if (tid_counter > max_cpu_num - 1) { /* FIXME */
    cpu = getnumChildThreads()%max_cpu_num;// same as worker thread
  } else {
    cpu = -1;// any cpu
  }
  VEO_DEBUG(ctx, "num_child_threads = %d", getnumChildThreads());
  pthread_mutex_unlock(&tid_counter_mutex);
  attr_v3.cpu = cpu;
  attr_v3.stack_sz = attr.getStacksize();
  VEO_DEBUG(ctx, "attributes: cpu %d, stack_sz 0x%lx", attr_v3.cpu, attr_v3.stack_sz);
  args.setOnStack(VEO_INTENT_IN, 0, reinterpret_cast<char *>(&attr_v3), sizeof(struct veo__thread_attribute_ver3));
  auto reqid = ctx->_callOpenContext(this, this->funcs.create_thread_with_attr, args);
  uintptr_t ret;
  int rv = ctx->callWaitResult(reqid, &ret);
  if (rv != VEO_COMMAND_OK) {
    VEO_ERROR(ctx, "openContext failed (%d)", rv);
    throw VEOException("request failed", ENOSYS);
  }
  return reinterpret_cast<ThreadContext *>(ret);
}

/**
 * @brief read data from VE memory
 * @param[out] dst buffer to store the data
 * @param src VEMVA to read
 * @param size size to transfer in byte
 * @return zero upon success; negative upon failure
 */
int ProcHandle::readMem(void *dst, uint64_t src, size_t size)
{
  std::lock_guard<std::mutex> lock(this->main_mutex);
  VEO_TRACE(nullptr, "readMem(%p, %#lx, %ld)", dst, src, size);
  auto id = this->worker->asyncReadMem(dst, src, size);
  uint64_t ret;
  int rv = this->worker->callWaitResult(id, &ret);
  VEO_ASSERT(rv == VEO_COMMAND_OK);
  return static_cast<int>(ret);
}

/**
 * @brief write data to VE memory
 * @param dst VEMVA to write the data
 * @param src buffer holding data to write
 * @param size size to transfer in byte
 * @return zero upon success; negative upon failure
 */
int ProcHandle::writeMem(uint64_t dst, const void *src, size_t size)
{
  std::lock_guard<std::mutex> lock(this->main_mutex);
  VEO_TRACE(nullptr, "writeMem(%#lx, %p, %ld)", dst, src, size);
  auto id = this->worker->asyncWriteMem(dst, src, size);
  uint64_t ret;
  int rv = this->worker->callWaitResult(id, &ret);
  VEO_ASSERT(rv == VEO_COMMAND_OK);
  return static_cast<int>(ret);
}

/**
 * @brief get the error that occurred from loadLibrary().
 * @param[out] ret_buff buffer to store the error.
 * @param size size of ret_buff
 * @return zero upon success; -1 upon failure
 */
int ProcHandle::loadLibraryError(char *ret_buff, size_t size)
{
  uint64_t buff;
  int64_t rv;
  char err_msg[ERR_MSG_LEN] = {'\0'};
  CallArgs args;

  try {
    buff = this->allocBuff(ERR_MSG_LEN);
  } catch (VEOException &e) {
    VEO_ERROR(nullptr,
	      "Error Detected on VE Buffer allocation procedure : %s", e.what());
    return -1;
  }
  if (buff == 0)
    return -1;

  VEO_DEBUG(nullptr, "VE Buffer = %p", (void *)buff);

  args.set(0, buff);
  args.set(1, ERR_MSG_LEN);
  try {
    rv = doOnContext(this->worker.get(),
			   this->funcs.load_library_err, args);
  } catch (VEOException &e) {
    VEO_ERROR(nullptr,
	      "Exception Detected in calling load_library_err : %s", e.what());
  }
  if (rv < 0) {
    VEO_ERROR(nullptr, "load_library_err failed, rv = %ld", rv);
    return -1;
  }

  rv = this->readMem((void *)err_msg, buff, ERR_MSG_LEN);
  if (rv != 0) {
    VEO_ERROR(nullptr, "readMem failed, rv = %ld", rv);
    return -1;
  }

  this->freeBuff(buff);
  memcpy(ret_buff, err_msg, (size < ERR_MSG_LEN) ? size : ERR_MSG_LEN);

  return 0;
}
} // namespace veo
