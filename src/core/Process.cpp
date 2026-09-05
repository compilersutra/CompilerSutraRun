#include "csrun/core/Process.h"

#include <array>
#include <cerrno>
#include <cstring>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace csrun {

Expected<ProcessResult> runProcess(const std::vector<std::string> &argv) {
  if (argv.empty())
    return Error::make(ErrorCode::Internal, "runProcess: empty argv");

  int outPipe[2];
  int errPipe[2];
  if (pipe(outPipe) != 0 || pipe(errPipe) != 0)
    return Error::make(ErrorCode::IoError, "failed to create pipes for subprocess");

  const pid_t pid = fork();
  if (pid < 0) {
    close(outPipe[0]);
    close(outPipe[1]);
    close(errPipe[0]);
    close(errPipe[1]);
    return Error::make(ErrorCode::IoError, "fork failed");
  }

  if (pid == 0) {
    dup2(outPipe[1], STDOUT_FILENO);
    dup2(errPipe[1], STDERR_FILENO);
    close(outPipe[0]);
    close(outPipe[1]);
    close(errPipe[0]);
    close(errPipe[1]);

    std::vector<char *> args;
    args.reserve(argv.size() + 1);
    for (const auto &a : argv)
      args.push_back(const_cast<char *>(a.c_str()));
    args.push_back(nullptr);
    execvp(args[0], args.data());
    _exit(127);
  }

  close(outPipe[1]);
  close(errPipe[1]);

  auto readAll = [](int fd) {
    std::string out;
    std::array<char, 4096> buf{};
    while (true) {
      const ssize_t n = read(fd, buf.data(), buf.size());
      if (n < 0) {
        if (errno == EINTR)
          continue;
        break;
      }
      if (n == 0)
        break;
      out.append(buf.data(), static_cast<std::size_t>(n));
    }
    return out;
  };

  ProcessResult result;
  result.stdoutText = readAll(outPipe[0]);
  result.stderrText = readAll(errPipe[0]);
  close(outPipe[0]);
  close(errPipe[0]);

  int status = 0;
  if (waitpid(pid, &status, 0) < 0)
    return Error::make(ErrorCode::IoError, "waitpid failed");
  if (WIFEXITED(status))
    result.exitCode = WEXITSTATUS(status);
  else
    result.exitCode = -1;
  return result;
}

} // namespace csrun
