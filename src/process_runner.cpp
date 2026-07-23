#include "az_dashboard/azure_cli.hpp"
#include "az_dashboard/windows_command.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace azdash {

namespace detail {

auto quote_windows_argument(std::string_view argument) -> std::string {
  if (argument.empty()) {
    return "\"\"";
  }

  const auto needs_quotes = argument.find_first_of(" \t\n\v\"") != std::string_view::npos;
  if (!needs_quotes) {
    return std::string(argument);
  }

  std::string quoted{"\""};
  auto backslashes = 0;
  for (const auto character : argument) {
    if (character == '\\') {
      ++backslashes;
      continue;
    }
    if (character == '"') {
      quoted.append(static_cast<std::size_t>(backslashes * 2 + 1), '\\');
      quoted += character;
      backslashes = 0;
      continue;
    }
    quoted.append(static_cast<std::size_t>(backslashes), '\\');
    backslashes = 0;
    quoted += character;
  }
  quoted.append(static_cast<std::size_t>(backslashes * 2), '\\');
  quoted += '"';
  return quoted;
}

// Always wraps a token in double quotes using the standard MSVC backslash
// rules. Unconditional quoting keeps cmd.exe from interpreting separators such
// as & | < > ( ) that would otherwise be significant on an unquoted token.
auto quote_windows_argument_always(std::string_view argument) -> std::string {
  std::string quoted{"\""};
  auto backslashes = 0;
  for (const auto character : argument) {
    if (character == '\\') {
      ++backslashes;
      continue;
    }
    if (character == '"') {
      quoted.append(static_cast<std::size_t>(backslashes * 2 + 1), '\\');
      quoted += character;
      backslashes = 0;
      continue;
    }
    quoted.append(static_cast<std::size_t>(backslashes), '\\');
    backslashes = 0;
    quoted += character;
  }
  quoted.append(static_cast<std::size_t>(backslashes * 2), '\\');
  quoted += '"';
  return quoted;
}

auto build_cmd_wrapped_command_line(std::string_view interpreter,
                                    std::string_view script_path,
                                    const std::vector<std::string>& arguments) -> std::string {
  std::string line{interpreter};
  line += " /d /s /c \"";
  line += quote_windows_argument_always(script_path);
  for (const auto& argument : arguments) {
    line += ' ';
    line += quote_windows_argument_always(argument);
  }
  line += '"';
  return line;
}

} // namespace detail

namespace {

constexpr auto kDefaultCommandTimeout = std::chrono::seconds{30};

auto effective_timeout(const ProcessRunnerOptions& options) -> std::chrono::milliseconds {
  if (options.timeout.count() > 0) {
    return options.timeout;
  }
  return std::chrono::duration_cast<std::chrono::milliseconds>(kDefaultCommandTimeout);
}

#ifdef _WIN32
class WindowsHandle {
public:
  explicit WindowsHandle(HANDLE handle = nullptr) noexcept : handle_(handle) {}
  WindowsHandle(const WindowsHandle&) = delete;
  auto operator=(const WindowsHandle&) -> WindowsHandle& = delete;

  WindowsHandle(WindowsHandle&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}
  auto operator=(WindowsHandle&& other) noexcept -> WindowsHandle& {
    if (this != &other) {
      reset(std::exchange(other.handle_, nullptr));
    }
    return *this;
  }

  ~WindowsHandle() {
    reset();
  }

  [[nodiscard]] auto get() const noexcept -> HANDLE {
    return handle_;
  }

  [[nodiscard]] auto valid() const noexcept -> bool {
    return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
  }

  auto reset(HANDLE handle = nullptr) noexcept -> void {
    if (valid()) {
      (void)::CloseHandle(handle_);
    }
    handle_ = handle;
  }

private:
  HANDLE handle_{nullptr};
};

class ProcThreadAttributeList {
public:
  ProcThreadAttributeList() = default;
  ProcThreadAttributeList(const ProcThreadAttributeList&) = delete;
  auto operator=(const ProcThreadAttributeList&) -> ProcThreadAttributeList& = delete;

  ~ProcThreadAttributeList() {
    if (attributes_ != nullptr) {
      ::DeleteProcThreadAttributeList(attributes_);
    }
  }

  [[nodiscard]] auto initialize(std::span<HANDLE> inherited_handles) -> bool {
    SIZE_T size = 0;
    (void)::InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    storage_.resize(size);
    attributes_ = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage_.data());
    if (::InitializeProcThreadAttributeList(attributes_, 1, 0, &size) == 0) {
      attributes_ = nullptr;
      storage_.clear();
      return false;
    }

    return ::UpdateProcThreadAttribute(attributes_, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited_handles.data(),
                                       inherited_handles.size() * sizeof(HANDLE), nullptr, nullptr) != 0;
  }

  [[nodiscard]] auto get() const noexcept -> LPPROC_THREAD_ATTRIBUTE_LIST {
    return attributes_;
  }

private:
  std::vector<unsigned char> storage_;
  LPPROC_THREAD_ATTRIBUTE_LIST attributes_{nullptr};
};

auto render_windows_command_line(const ProcessCommand& command) -> std::string {
  auto rendered = detail::quote_windows_argument(command.executable);
  for (const auto& argument : command.arguments) {
    rendered += ' ';
    rendered += detail::quote_windows_argument(argument);
  }
  return rendered;
}

auto ends_with_ci(std::string_view value, std::string_view suffix) -> bool {
  if (value.size() < suffix.size()) {
    return false;
  }
  const auto offset = value.size() - suffix.size();
  for (std::size_t index = 0; index < suffix.size(); ++index) {
    if (std::tolower(static_cast<unsigned char>(value[offset + index])) !=
        std::tolower(static_cast<unsigned char>(suffix[index]))) {
      return false;
    }
  }
  return true;
}

auto comspec_path() -> std::string {
  std::array<char, 1024> buffer{};
  const auto length = ::GetEnvironmentVariableA("ComSpec", buffer.data(), static_cast<DWORD>(buffer.size()));
  if (length > 0 && length < buffer.size()) {
    return std::string(buffer.data(), length);
  }
  return "cmd.exe";
}

auto search_path_for(const std::string& file, const char* extension) -> std::string {
  std::vector<char> buffer(32768);
  LPSTR file_part = nullptr;
  const auto length =
      ::SearchPathA(nullptr, file.c_str(), extension, static_cast<DWORD>(buffer.size()), buffer.data(), &file_part);
  if (length == 0 || length >= buffer.size()) {
    return {};
  }
  return std::string(buffer.data(), length);
}

auto has_file_extension(std::string_view name) -> bool {
  const auto slash = name.find_last_of("\\/");
  const auto base = (slash == std::string_view::npos) ? name : name.substr(slash + 1);
  return base.find('.') != std::string_view::npos;
}

auto probe_pathext(const std::string& executable) -> std::string {
  std::array<char, 1024> pathext_buffer{};
  const auto length =
      ::GetEnvironmentVariableA("PATHEXT", pathext_buffer.data(), static_cast<DWORD>(pathext_buffer.size()));
  const std::string pathext = (length > 0 && length < pathext_buffer.size())
                                  ? std::string(pathext_buffer.data(), length)
                                  : std::string(".COM;.EXE;.BAT;.CMD");

  std::size_t start = 0;
  while (start <= pathext.size()) {
    const auto separator = pathext.find(';', start);
    const auto extension = pathext.substr(start, separator == std::string::npos ? std::string::npos : separator - start);
    if (!extension.empty()) {
      if (auto hit = search_path_for(executable, extension.c_str()); !hit.empty()) {
        return hit;
      }
    }
    if (separator == std::string::npos) {
      break;
    }
    start = separator + 1;
  }
  return {};
}

// Resolves an executable name to a full path the way a shell would, consulting
// PATH and PATHEXT. CreateProcess itself only appends ".exe", so this is what
// lets us find and launch batch launchers such as the Azure CLI's az.cmd.
//
// PATHEXT extensions are probed before a bare, extensionless match: the Azure
// CLI ships both az.cmd (the Windows launcher) and an extensionless az (a shell
// script) in the same directory, and only the former can be executed here.
auto resolve_windows_executable(const std::string& executable) -> std::string {
  if (has_file_extension(executable)) {
    if (auto direct = search_path_for(executable, nullptr); !direct.empty()) {
      return direct;
    }
  }

  if (auto hit = probe_pathext(executable); !hit.empty()) {
    return hit;
  }

  if (auto direct = search_path_for(executable, nullptr); !direct.empty()) {
    return direct;
  }
  return {};
}

auto make_windows_pipe() -> std::pair<WindowsHandle, WindowsHandle> {
  SECURITY_ATTRIBUTES security_attributes{};
  security_attributes.nLength = sizeof(SECURITY_ATTRIBUTES);
  security_attributes.bInheritHandle = TRUE;

  HANDLE read_handle = nullptr;
  HANDLE write_handle = nullptr;
  if (::CreatePipe(&read_handle, &write_handle, &security_attributes, 0) == 0) {
    throw std::runtime_error("failed to create process pipe");
  }

  WindowsHandle read{read_handle};
  WindowsHandle write{write_handle};
  if (::SetHandleInformation(read.get(), HANDLE_FLAG_INHERIT, 0) == 0) {
    throw std::runtime_error("failed to configure process pipe");
  }
  return {std::move(read), std::move(write)};
}

auto pipe_has_data(const WindowsHandle& handle) -> bool {
  DWORD available = 0;
  return ::PeekNamedPipe(handle.get(), nullptr, 0, nullptr, &available, nullptr) != 0 && available > 0;
}

auto read_available(WindowsHandle& handle, std::string& output) -> bool {
  DWORD available = 0;
  if (::PeekNamedPipe(handle.get(), nullptr, 0, nullptr, &available, nullptr) == 0) {
    handle.reset();
    return false;
  }
  if (available == 0) {
    return true;
  }

  std::array<char, 4096> buffer{};
  while (available > 0) {
    DWORD bytes_read = 0;
    const auto bytes_to_read = std::min<DWORD>(available, static_cast<DWORD>(buffer.size()));
    if (::ReadFile(handle.get(), buffer.data(), bytes_to_read, &bytes_read, nullptr) == 0 || bytes_read == 0) {
      handle.reset();
      return false;
    }
    output.append(buffer.data(), static_cast<std::size_t>(bytes_read));
    available -= bytes_read;
  }
  return true;
}

auto exit_code_from_process(const WindowsHandle& process, bool timed_out) -> int {
  if (timed_out) {
    return -1;
  }
  DWORD exit_code = 1;
  if (::GetExitCodeProcess(process.get(), &exit_code) == 0) {
    return 1;
  }
  return static_cast<int>(exit_code);
}

auto run_process(const ProcessCommand& command, const ProcessRunnerOptions& options) -> CommandResult {
  if (command.executable.empty()) {
    return {1, "", "executable is required"};
  }

  auto [stdout_read, stdout_write] = make_windows_pipe();
  auto [stderr_read, stderr_write] = make_windows_pipe();
  SECURITY_ATTRIBUTES stdin_security_attributes{};
  stdin_security_attributes.nLength = sizeof(SECURITY_ATTRIBUTES);
  stdin_security_attributes.bInheritHandle = TRUE;
  WindowsHandle stdin_handle{::CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                           &stdin_security_attributes, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
  if (!stdin_handle.valid()) {
    return {1, "", "failed to open null stdin"};
  }

  // On Windows the Azure CLI ships as az.cmd, and CreateProcess cannot launch a
  // batch script directly. Resolve the real target and, when it is a .cmd/.bat
  // launcher, run it through cmd.exe with typed arguments kept inert.
  const auto resolved_executable = resolve_windows_executable(command.executable);
  std::string application_name;
  std::string command_line;
  if (!resolved_executable.empty() &&
      (ends_with_ci(resolved_executable, ".cmd") || ends_with_ci(resolved_executable, ".bat"))) {
    application_name = comspec_path();
    command_line = detail::build_cmd_wrapped_command_line(application_name, resolved_executable, command.arguments);
  } else {
    application_name = resolved_executable;
    auto direct_command = command;
    if (!resolved_executable.empty()) {
      direct_command.executable = resolved_executable;
    }
    command_line = render_windows_command_line(direct_command);
  }

  std::array<HANDLE, 3> inherited_handles{stdin_handle.get(), stdout_write.get(), stderr_write.get()};
  ProcThreadAttributeList handle_list;
  if (!handle_list.initialize(inherited_handles)) {
    return {1, "", "failed to configure inherited process handles"};
  }

  STARTUPINFOEXA startup_info{};
  startup_info.StartupInfo.cb = sizeof(STARTUPINFOEXA);
  startup_info.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  startup_info.StartupInfo.hStdInput = stdin_handle.get();
  startup_info.StartupInfo.hStdOutput = stdout_write.get();
  startup_info.StartupInfo.hStdError = stderr_write.get();
  startup_info.lpAttributeList = handle_list.get();

  PROCESS_INFORMATION process_info{};
  const LPCSTR application = application_name.empty() ? nullptr : application_name.c_str();
  if (::CreateProcessA(application, command_line.data(), nullptr, nullptr, TRUE,
                       CREATE_NEW_PROCESS_GROUP | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr,
                       &startup_info.StartupInfo, &process_info) == 0) {
    return {1, "", "failed to create process"};
  }

  WindowsHandle process{process_info.hProcess};
  WindowsHandle thread{process_info.hThread};
  WindowsHandle job{::CreateJobObjectA(nullptr, nullptr)};
  const auto job_assigned = job.valid() && ::AssignProcessToJobObject(job.get(), process.get()) != 0;
  stdout_write.reset();
  stderr_write.reset();

  std::string stdout_text;
  std::string stderr_text;
  auto stdout_open = true;
  auto stderr_open = true;
  auto exited = false;
  auto timed_out = false;

  const auto deadline = std::chrono::steady_clock::now() + effective_timeout(options);
  while (stdout_open || stderr_open || !exited) {
    if (!exited) {
      const auto wait_result = ::WaitForSingleObject(process.get(), 10);
      if (wait_result == WAIT_OBJECT_0 || wait_result == WAIT_FAILED) {
        exited = true;
      }
    }

    if (!timed_out && (stdout_open || stderr_open || !exited) && std::chrono::steady_clock::now() >= deadline) {
      if (job_assigned) {
        (void)::TerminateJobObject(job.get(), 1);
      } else {
        (void)::TerminateProcess(process.get(), 1);
      }
      (void)::WaitForSingleObject(process.get(), INFINITE);
      timed_out = true;
      exited = true;
      stdout_read.reset();
      stderr_read.reset();
      stdout_open = false;
      stderr_open = false;
    }

    if (stdout_open) {
      stdout_open = read_available(stdout_read, stdout_text);
    }
    if (stderr_open) {
      stderr_open = read_available(stderr_read, stderr_text);
    }

    if (exited) {
      if (stdout_open && !pipe_has_data(stdout_read)) {
        stdout_read.reset();
        stdout_open = false;
      }
      if (stderr_open && !pipe_has_data(stderr_read)) {
        stderr_read.reset();
        stderr_open = false;
      }
    }
  }

  return {exit_code_from_process(process, timed_out), stdout_text, stderr_text, timed_out};
}
#else
class FileDescriptor {
public:
  explicit FileDescriptor(int fd = -1) noexcept : fd_(fd) {}
  FileDescriptor(const FileDescriptor&) = delete;
  auto operator=(const FileDescriptor&) -> FileDescriptor& = delete;

  FileDescriptor(FileDescriptor&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
  auto operator=(FileDescriptor&& other) noexcept -> FileDescriptor& {
    if (this != &other) {
      reset(std::exchange(other.fd_, -1));
    }
    return *this;
  }

  ~FileDescriptor() {
    reset();
  }

  [[nodiscard]] auto get() const noexcept -> int {
    return fd_;
  }

  [[nodiscard]] auto valid() const noexcept -> bool {
    return fd_ >= 0;
  }

  auto reset(int fd = -1) noexcept -> void {
    if (fd_ >= 0) {
      (void)::close(fd_);
    }
    fd_ = fd;
  }

private:
  int fd_{-1};
};

auto make_pipe() -> std::pair<FileDescriptor, FileDescriptor> {
  std::array<int, 2> descriptors{-1, -1};
  if (::pipe(descriptors.data()) == -1) {
    throw std::runtime_error(std::string("failed to create process pipe: ") + std::strerror(errno));
  }
  return {FileDescriptor{descriptors[0]}, FileDescriptor{descriptors[1]}};
}

auto set_nonblocking(const FileDescriptor& descriptor) -> bool {
  const auto flags = ::fcntl(descriptor.get(), F_GETFL, 0);
  if (flags == -1) {
    return false;
  }
  return ::fcntl(descriptor.get(), F_SETFL, flags | O_NONBLOCK) != -1;
}

auto build_argv(const ProcessCommand& command) -> std::vector<char*> {
  std::vector<char*> argv;
  argv.reserve(command.arguments.size() + 2);
  argv.push_back(const_cast<char*>(command.executable.c_str()));
  for (const auto& argument : command.arguments) {
    argv.push_back(const_cast<char*>(argument.c_str()));
  }
  argv.push_back(nullptr);
  return argv;
}

auto read_available(FileDescriptor& descriptor, std::string& output) -> bool {
  std::array<char, 4096> buffer{};
  for (;;) {
    const auto bytes_read = ::read(descriptor.get(), buffer.data(), buffer.size());
    if (bytes_read > 0) {
      output.append(buffer.data(), static_cast<std::size_t>(bytes_read));
      continue;
    }
    if (bytes_read == 0) {
      descriptor.reset();
      return false;
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return true;
    }
    descriptor.reset();
    return false;
  }
}

auto wait_for_child(pid_t pid, int& status, int options) -> pid_t {
  for (;;) {
    const auto waited = ::waitpid(pid, &status, options);
    if (waited != -1 || errno != EINTR) {
      return waited;
    }
  }
}

auto exit_code_from_status(int status, bool timed_out) -> int {
  if (timed_out) {
    return -1;
  }
  if (WIFEXITED(status)) {
    return WEXITSTATUS(status);
  }
  if (WIFSIGNALED(status)) {
    return 128 + WTERMSIG(status);
  }
  return 1;
}

auto run_process(const ProcessCommand& command, const ProcessRunnerOptions& options) -> CommandResult {
  if (command.executable.empty()) {
    return {1, "", "executable is required"};
  }

  auto [stdout_read, stdout_write] = make_pipe();
  auto [stderr_read, stderr_write] = make_pipe();

  const auto pid = ::fork();
  if (pid == -1) {
    return {1, "", std::string("failed to fork process: ") + std::strerror(errno)};
  }

  if (pid == 0) {
    (void)::setpgid(0, 0);
    stdout_read.reset();
    stderr_read.reset();

    if (::dup2(stdout_write.get(), STDOUT_FILENO) == -1 || ::dup2(stderr_write.get(), STDERR_FILENO) == -1) {
      _exit(127);
    }

    stdout_write.reset();
    stderr_write.reset();

    auto argv = build_argv(command);
    ::execvp(command.executable.c_str(), argv.data());
    const std::string message = std::string("failed to execute process: ") + std::strerror(errno) + "\n";
    (void)::write(STDERR_FILENO, message.data(), message.size());
    _exit(127);
  }

  stdout_write.reset();
  stderr_write.reset();
  (void)::setpgid(pid, pid);

  if (!set_nonblocking(stdout_read) || !set_nonblocking(stderr_read)) {
    (void)::kill(pid, SIGKILL);
    int status = 0;
    (void)wait_for_child(pid, status, 0);
    return {1, "", "failed to configure process pipes"};
  }

  std::string stdout_text;
  std::string stderr_text;
  auto stdout_open = stdout_read.valid();
  auto stderr_open = stderr_read.valid();
  auto exited = false;
  auto timed_out = false;
  auto status = 0;

  const auto timeout = effective_timeout(options);
  const auto deadline = std::chrono::steady_clock::now() + timeout;

  while (stdout_open || stderr_open || !exited) {
    if (!exited) {
      const auto waited = wait_for_child(pid, status, WNOHANG);
      if (waited == pid) {
        exited = true;
      } else if (waited == -1) {
        status = 1;
        exited = true;
      }
    }

    if (!timed_out && (stdout_open || stderr_open || !exited) && std::chrono::steady_clock::now() >= deadline) {
      (void)::kill(-pid, SIGKILL);
      (void)::kill(pid, SIGKILL);
      timed_out = true;
      if (!exited) {
        (void)wait_for_child(pid, status, 0);
      }
      exited = true;
      stdout_read.reset();
      stderr_read.reset();
      stdout_open = false;
      stderr_open = false;
    }

    std::array<pollfd, 2> poll_descriptors{};
    auto descriptor_count = nfds_t{0};
    if (stdout_open) {
      poll_descriptors[descriptor_count++] = {.fd = stdout_read.get(), .events = POLLIN | POLLHUP | POLLERR};
    }
    if (stderr_open) {
      poll_descriptors[descriptor_count++] = {.fd = stderr_read.get(), .events = POLLIN | POLLHUP | POLLERR};
    }

    if (descriptor_count == 0) {
      if (!exited) {
        (void)wait_for_child(pid, status, 0);
        exited = true;
      }
      continue;
    }

    auto poll_timeout_ms = 0;
    if (!timed_out) {
      const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
          deadline - std::chrono::steady_clock::now());
      const auto remaining_ms = remaining.count();
      poll_timeout_ms = static_cast<int>(
          std::clamp(remaining_ms, decltype(remaining_ms){0}, decltype(remaining_ms){100}));
    }

    const auto ready = ::poll(poll_descriptors.data(), descriptor_count, poll_timeout_ms);
    if (ready == -1) {
      if (errno == EINTR) {
        continue;
      }
      stderr_text += std::string("failed to poll process pipes: ") + std::strerror(errno);
      break;
    }

    auto poll_index = nfds_t{0};
    if (stdout_open) {
      const auto revents = poll_descriptors[poll_index++].revents;
      if ((revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) != 0) {
        stdout_open = read_available(stdout_read, stdout_text);
      }
    }
    if (stderr_open) {
      const auto revents = poll_descriptors[poll_index].revents;
      if ((revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) != 0) {
        stderr_open = read_available(stderr_read, stderr_text);
      }
    }
  }

  return {exit_code_from_status(status, timed_out), stdout_text, stderr_text, timed_out};
}
#endif

} // namespace

auto ShellCommandRunner::run(const ProcessCommand& command, const ProcessRunnerOptions& options) const -> CommandResult {
  try {
    return run_process(command, options);
  } catch (const std::exception& error) {
    return {1, "", error.what()};
  }
}

} // namespace azdash
