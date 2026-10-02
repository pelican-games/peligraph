// Process-wide GPU test lease.  This is test infrastructure: it deliberately
// has no dependency on Pelican production code.
//
// Contract (WP385 v3):
//   * Every CTest test carrying the `gpu` label runs as
//     `pelican_gpu_lease_run [--timeout-seconds N] -- <command...>`.
//   * The lease is machine-wide on Windows (`Global\` namespace) and, on
//     POSIX, scoped to one UID plus one `$XDG_RUNTIME_DIR` (named limit).
//   * Waiting longer than the deadline is a failure, never a skip:
//     `gpu_lease_timeout@1{holder_pid=...,waited_seconds=...}` with exit 124.
//   * The launcher owns the whole process tree it starts, so a launcher that
//     is killed cannot leave a GPU-holding grandchild behind.
//   * Acquire/release are appended to a per-run log with a monotonic clock so
//     two processes can be shown not to overlap.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#ifdef _WIN32
#  define NOMINMAX
#  include <windows.h>
#else
#  include <fcntl.h>
#  include <signal.h>
#  include <sys/file.h>
#  include <sys/types.h>
#  include <sys/wait.h>
#  include <unistd.h>
#endif

namespace {
constexpr int kDefaultTimeoutSeconds = 1200;
constexpr char kDefaultLeaseName[] = "pelican_gpu_lease_v1";

// A test-scoped lease name keeps the lease's own acceptance tests off the one
// real GPU lease; production GPU tests never set it.
std::string lease_name() {
  const char* name = std::getenv("PELICAN_GPU_LEASE_NAME");
  return (name && *name) ? std::string(name) : std::string(kDefaultLeaseName);
}

long long monotonic_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
}

int timeout_seconds(int argc, char** argv, int& command_at) {
  int timeout = kDefaultTimeoutSeconds;
  const char* value = std::getenv("PELICAN_GPU_LEASE_TIMEOUT_SECONDS");
  if (value && *value) timeout = std::atoi(value);
  command_at = 1;
  if (argc > 3 && std::strcmp(argv[1], "--timeout-seconds") == 0) {
    timeout = std::atoi(argv[2]);
    command_at = 3;
  }
  if (command_at < argc && std::strcmp(argv[command_at], "--") == 0) ++command_at;
  return timeout > 0 ? timeout : kDefaultTimeoutSeconds;
}

// holder token = pid + process start time + in-process sequence (decision 13).
std::string token() {
#ifdef _WIN32
  FILETIME created{}, exited{}, kernel{}, user{};
  GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
  const unsigned long long started =
      (static_cast<unsigned long long>(created.dwHighDateTime) << 32) |
      created.dwLowDateTime;
  return std::to_string(GetCurrentProcessId()) + "-" + std::to_string(started) + "-1";
#else
  return std::to_string(getpid()) + "-" + std::to_string(
      std::chrono::steady_clock::now().time_since_epoch().count()) + "-1";
#endif
}

std::string log_directory() {
  if (const char* dir = std::getenv("PELICAN_GPU_LEASE_LOG_DIR"); dir && *dir)
    return dir;
#ifdef _WIN32
  if (const char* dir = std::getenv("TEMP"); dir && *dir) return dir;
  return ".";
#else
  if (const char* dir = std::getenv("XDG_RUNTIME_DIR"); dir && *dir) return dir;
  return "/tmp";
#endif
}

// TS-1: every GPU test and every worktree share one shader disk cache. The
// engine names cache entries by content (sources, compiler, target, options),
// so a fresh test project starts with the shaders earlier tests compiled.
// A cache directory the caller already chose is kept; when none can be made,
// the variable stays unset and each project keeps its own cache.
void prepare_shader_cache_directory() {
  constexpr char variable[] = "PELICAN_SHADER_CACHE_DIR";
  if (const char* set = std::getenv(variable); set && *set) return;
#ifdef _WIN32
  const char* base = std::getenv("LOCALAPPDATA");
  if (!base || !*base) return;
  const std::filesystem::path directory =
      std::filesystem::path(base) / "pelican" / "test_shader_cache";
#else
  std::filesystem::path directory;
  if (const char* cache = std::getenv("XDG_CACHE_HOME"); cache && *cache)
    directory = std::filesystem::path(cache) / "pelican" / "test_shader_cache";
  else if (const char* home = std::getenv("HOME"); home && *home)
    directory = std::filesystem::path(home) / ".cache" / "pelican" / "test_shader_cache";
  else
    return;
#endif
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  if (error || !std::filesystem::is_directory(directory, error) || error) return;
#ifdef _WIN32
  SetEnvironmentVariableA(variable, directory.string().c_str());
#else
  setenv(variable, directory.string().c_str(), 1);
#endif
}

void log_event(const std::string& name, const std::string& own,
               const char* event, long long pid) {
  std::ofstream log(log_directory() + "/" + name + "." + std::to_string(pid) + ".log",
                    std::ios::app);
  if (!log) return;
  log << own << ' ' << event << ' ' << monotonic_ns() << '\n';
}

#ifdef _WIN32
struct Holder { DWORD pid; char token[128]; };

std::string quote(const char* arg) {
  std::string out{"\""};
  unsigned slashes = 0;
  for (const char* p = arg; *p; ++p) {
    if (*p == '\\') { ++slashes; continue; }
    if (*p == '\"') out.append(slashes * 2 + 1, '\\');
    else out.append(slashes, '\\');
    slashes = 0;
    out += *p;
  }
  out.append(slashes * 2, '\\'); out += '\"'; return out;
}

std::string command_line(int argc, char** argv, int at) {
  std::string command;
  for (int i = at; i < argc; ++i) {
    if (!command.empty()) command += ' ';
    command += quote(argv[i]);
  }
  return command;
}

int run_unleased(int argc, char** argv, int at) {
  std::string command = command_line(argc, argv, at);
  std::vector<char> mutable_command(command.begin(), command.end());
  mutable_command.push_back('\0');
  STARTUPINFOA si{}; si.cb = sizeof(si); PROCESS_INFORMATION pi{};
  if (!CreateProcessA(nullptr, mutable_command.data(), nullptr, nullptr, FALSE, 0,
                      nullptr, nullptr, &si, &pi)) {
    std::cerr << "gpu_lease_child_start_failed@1\n";
    return 126;
  }
  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD code = 1; GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
  return static_cast<int>(code);
}

struct Lease {
  HANDLE mutex = nullptr;
  HANDLE mapping = nullptr;
  Holder* holder = nullptr;
  void close() {
    if (holder) UnmapViewOfFile(holder);
    if (mapping) CloseHandle(mapping);
    if (mutex) CloseHandle(mutex);
    holder = nullptr; mapping = nullptr; mutex = nullptr;
  }
};

int run_windows(int argc, char** argv, int at, int timeout) {
  const std::string name = lease_name();
  const std::string own = token();
  Lease lease;
  lease.mutex = CreateMutexA(nullptr, FALSE, ("Global\\" + name).c_str());
  if (!lease.mutex) { std::cerr << "gpu_lease_create_failed@1\n"; return 125; }
  // The lease itself is the Global mutex.  The holder record is only the
  // diagnostic half (holder pid + token), and creating a *section* object in
  // the Global namespace needs SeCreateGlobalPrivilege, which an unelevated
  // test run does not have.  Fall back to the session namespace so the
  // timeout message keeps naming a pid instead of reporting 0.
  lease.mapping = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                     sizeof(Holder), ("Global\\" + name + "_holder").c_str());
  if (!lease.mapping) {
    lease.mapping = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                       sizeof(Holder), (name + "_holder").c_str());
  }
  lease.holder = lease.mapping ? static_cast<Holder*>(
      MapViewOfFile(lease.mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Holder))) : nullptr;
  const DWORD wait = WaitForSingleObject(lease.mutex, static_cast<DWORD>(timeout) * 1000U);
  if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) {
    const DWORD pid = lease.holder ? lease.holder->pid : 0;
    std::cerr << "gpu_lease_timeout@1{holder_pid=" << pid
              << ",waited_seconds=" << timeout << "}\n";
    lease.close();
    return 124;
  }
  if (lease.holder) {
    lease.holder->pid = GetCurrentProcessId();
    std::snprintf(lease.holder->token, sizeof(lease.holder->token), "%s", own.c_str());
  }
  log_event(name, own, "acquired", GetCurrentProcessId());

  // Kill-on-close job: everything the child starts dies with this launcher,
  // including when CTest terminates the launcher on its own timeout.
  HANDLE job = CreateJobObjectA(nullptr, nullptr);
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  if (!job || !SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                       &limits, sizeof(limits))) {
    std::cerr << (job ? "gpu_lease_job_configure_failed@1\n" : "gpu_lease_job_create_failed@1\n");
    if (job) CloseHandle(job);
    ReleaseMutex(lease.mutex); lease.close();
    return 125;
  }
  std::string command = command_line(argc, argv, at);
  std::vector<char> mutable_command(command.begin(), command.end());
  mutable_command.push_back('\0');
  STARTUPINFOA si{}; si.cb = sizeof(si); PROCESS_INFORMATION pi{};
  const BOOL started = CreateProcessA(nullptr, mutable_command.data(), nullptr, nullptr,
                                      FALSE, CREATE_SUSPENDED, nullptr, nullptr, &si, &pi);
  if (!started) {
    std::cerr << "gpu_lease_child_start_failed@1\n";
    CloseHandle(job); ReleaseMutex(lease.mutex); lease.close();
    return 126;
  }
  if (!AssignProcessToJobObject(job, pi.hProcess)) {
    std::cerr << "gpu_lease_job_assign_failed@1\n";
    TerminateProcess(pi.hProcess, 125);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess); CloseHandle(job);
    ReleaseMutex(lease.mutex); lease.close();
    return 125;
  }
  ResumeThread(pi.hThread);
  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD code = 1; GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hThread); CloseHandle(pi.hProcess); CloseHandle(job);

  if (lease.holder && std::strncmp(lease.holder->token, own.c_str(), sizeof(lease.holder->token)) != 0) {
    std::cerr << "gpu_lease_collision@1{expected=" << own
              << ",observed=" << lease.holder->token << "}\n";
    code = 125;
  }
  if (lease.holder) lease.holder->pid = 0;
  log_event(name, own, "released", GetCurrentProcessId());
  ReleaseMutex(lease.mutex);
  lease.close();
  return static_cast<int>(code);
}
#else
volatile sig_atomic_t child_group = -1;
void forward_signal(int signal) { if (child_group > 0) kill(-child_group, signal); }

int run_posix(int argc, char** argv, int at, int timeout) {
  const char* runtime = std::getenv("XDG_RUNTIME_DIR");
  if (!runtime || !*runtime) { std::cerr << "gpu_lease_runtime_dir_missing@1\n"; return 125; }
  const std::string name = lease_name();
  const std::string root(runtime), lock = root + "/" + name, sentinel = lock + ".sentinel";
  int fd = open(lock.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
  if (fd < 0) { std::cerr << "gpu_lease_open_failed@1\n"; return 125; }
  const auto began = std::chrono::steady_clock::now();
  while (flock(fd, LOCK_EX | LOCK_NB) != 0) {
    if (std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - began).count() >= timeout) {
      std::ifstream h(sentinel); std::string owner; std::getline(h, owner);
      const std::string holder_pid = owner.substr(0, owner.find('-'));
      std::cerr << "gpu_lease_timeout@1{holder_pid=" << (holder_pid.empty() ? "0" : holder_pid)
                << ",waited_seconds=" << timeout << "}\n";
      close(fd); return 124;
    }
    usleep(100000);
  }
  const auto own = token();
  { std::ofstream s(sentinel, std::ios::trunc); s << own << '\n'; }
  log_event(name, own, "acquired", getpid());
  pid_t child = fork();
  if (child == 0) { setsid(); execvp(argv[at], argv + at); _exit(127); }
  child_group = child; signal(SIGTERM, forward_signal); signal(SIGINT, forward_signal);
  int status = 1; waitpid(child, &status, 0);
  int code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
  { std::ifstream s(sentinel); std::string observed; std::getline(s, observed);
    if (observed != own) {
      std::cerr << "gpu_lease_collision@1{expected=" << own
                << ",observed=" << observed << "}\n";
      code = 125;
    } }
  log_event(name, own, "released", getpid());
  flock(fd, LOCK_UN); close(fd);
  return code;
}
#endif
}

int main(int argc, char** argv) {
  int at; const int timeout = timeout_seconds(argc, argv, at);
  if (at >= argc) {
    std::cerr << "usage: pelican_gpu_lease_run [--timeout-seconds N] -- <command> [args...]\n";
    return 2;
  }
  prepare_shader_cache_directory();
  if (const char* mode = std::getenv("PELICAN_GPU_LEASE"); mode && std::strcmp(mode, "off") == 0) {
#ifdef _WIN32
    return run_unleased(argc, argv, at);
#else
    execvp(argv[at], argv + at); return 127;
#endif
  }
#ifdef _WIN32
  return run_windows(argc, argv, at, timeout);
#else
  return run_posix(argc, argv, at, timeout);
#endif
}
