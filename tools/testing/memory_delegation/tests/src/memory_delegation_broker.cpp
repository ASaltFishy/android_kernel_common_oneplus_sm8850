// SPDX-License-Identifier: GPL-2.0
//
// Minimal userspace broker for Scudo SharedArena Android tests.
//
// This daemon is the only process that creates and owns per-CPU arena backing
// memfds. Client processes connect to the abstract unix socket below and receive
// a duplicated arena fd via SCM_RIGHTS. Keep the wire protocol in lockstep with
// compiler-rt/lib/scudo/standalone/shared_arena_linux.cpp.

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

constexpr char kBrokerName[] = "scudo_shared_arena_broker";
constexpr uint32_t kBrokerMagic = 0x5341524EU; // "SARN"
constexpr uint32_t kArenaMaxCores = 16;
constexpr size_t kArenaCapacityPerCore = 512ULL * 1024 * 1024;

struct BrokerRequest {
  uint32_t Magic;
  uint32_t CoreId;
};

struct BrokerReply {
  int32_t Status;
  uint32_t Reserved;
};

static_assert(sizeof(BrokerRequest) == 8, "broker request ABI drift");
static_assert(sizeof(BrokerReply) == 8, "broker reply ABI drift");

volatile sig_atomic_t StopRequested;
bool Trace;

void trace(const char *Format, ...) {
  if (!Trace)
    return;

  va_list Args;
  va_start(Args, Format);
  fprintf(stderr, "[memory_delegation_broker] ");
  vfprintf(stderr, Format, Args);
  fprintf(stderr, "\n");
  va_end(Args);
}

void onSignal(int) {
  StopRequested = 1;
}

void brokerSockaddr(sockaddr_un &Addr, socklen_t &AddrLen) {
  memset(&Addr, 0, sizeof(Addr));
  Addr.sun_family = AF_UNIX;
  Addr.sun_path[0] = '\0';
  memcpy(Addr.sun_path + 1, kBrokerName, sizeof(kBrokerName) - 1);
  AddrLen = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + 1 +
                                   sizeof(kBrokerName) - 1);
}

bool readExact(int Fd, void *Buf, size_t Size) {
  char *P = static_cast<char *>(Buf);
  while (Size != 0) {
    const ssize_t N = read(Fd, P, Size);
    if (N < 0) {
      if (errno == EINTR)
        continue;
      return false;
    }
    if (N == 0)
      return false;
    P += N;
    Size -= static_cast<size_t>(N);
  }
  return true;
}

bool sendReply(int Sock, int Status, int FdToSend) {
  BrokerReply Reply = {};
  Reply.Status = Status;

  iovec Iov = {};
  Iov.iov_base = &Reply;
  Iov.iov_len = sizeof(Reply);

  alignas(struct cmsghdr) char Control[CMSG_SPACE(sizeof(int))];
  memset(Control, 0, sizeof(Control));

  msghdr Msg = {};
  Msg.msg_iov = &Iov;
  Msg.msg_iovlen = 1;
  if (FdToSend >= 0) {
    Msg.msg_control = Control;
    Msg.msg_controllen = sizeof(Control);
    cmsghdr *Cmsg = CMSG_FIRSTHDR(&Msg);
    Cmsg->cmsg_level = SOL_SOCKET;
    Cmsg->cmsg_type = SCM_RIGHTS;
    Cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    *reinterpret_cast<int *>(CMSG_DATA(Cmsg)) = FdToSend;
    Msg.msg_controllen = Cmsg->cmsg_len;
  }

  while (sendmsg(Sock, &Msg, MSG_NOSIGNAL) < 0) {
    if (errno == EINTR)
      continue;
    return false;
  }
  return true;
}

int createArenaBacking(uint32_t CoreId) {
#if defined(SYS_memfd_create)
  char Name[64];
  snprintf(Name, sizeof(Name), "scudo_arena_%u", CoreId);
  const int Fd = static_cast<int>(syscall(SYS_memfd_create, Name, 0));
  if (Fd < 0) {
    trace("memfd_create core=%u failed errno=%d", CoreId, errno);
    return -1;
  }
  if (ftruncate(Fd, static_cast<off_t>(kArenaCapacityPerCore)) != 0) {
    trace("ftruncate core=%u failed errno=%d", CoreId, errno);
    close(Fd);
    return -1;
  }
  trace("created core=%u fd=%d", CoreId, Fd);
  return Fd;
#else
  (void)CoreId;
  fprintf(stderr, "memory_delegation_broker: SYS_memfd_create unavailable\n");
  return -1;
#endif
}

uint32_t defaultNumCores() {
  long Online = sysconf(_SC_NPROCESSORS_ONLN);
  if (Online <= 0)
    Online = 1;
  if (Online > static_cast<long>(kArenaMaxCores))
    Online = kArenaMaxCores;
  return static_cast<uint32_t>(Online);
}

bool parseU32(const char *Text, uint32_t &Out) {
  char *End = nullptr;
  errno = 0;
  const unsigned long Value = strtoul(Text, &End, 10);
  if (errno != 0 || End == Text || (End && *End != '\0') ||
      Value > UINT32_MAX)
    return false;
  Out = static_cast<uint32_t>(Value);
  return true;
}

void usage(const char *Argv0) {
  fprintf(stderr,
          "usage: %s [--num-cores N] [--trace]\n"
          "  --num-cores N  number of arena fds to create (1..%u)\n"
          "  --trace        print broker events to stderr\n",
          Argv0, kArenaMaxCores);
}

} // namespace

int main(int argc, char **argv) {
  uint32_t NumCores = defaultNumCores();

  for (int I = 1; I < argc; ++I) {
    if (strcmp(argv[I], "--trace") == 0) {
      Trace = true;
      continue;
    }
    if (strcmp(argv[I], "--num-cores") == 0 && I + 1 < argc) {
      if (!parseU32(argv[++I], NumCores) || NumCores == 0 ||
          NumCores > kArenaMaxCores) {
        usage(argv[0]);
        return 2;
      }
      continue;
    }
    usage(argv[0]);
    return 2;
  }

  int ArenaFds[kArenaMaxCores];
  for (uint32_t I = 0; I < kArenaMaxCores; ++I)
    ArenaFds[I] = -1;
  for (uint32_t I = 0; I < NumCores; ++I) {
    ArenaFds[I] = createArenaBacking(I);
    if (ArenaFds[I] < 0)
      return 1;
  }

  const int ListenFd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (ListenFd < 0) {
    fprintf(stderr, "memory_delegation_broker: socket failed errno=%d\n", errno);
    return 1;
  }

  sockaddr_un Addr;
  socklen_t AddrLen;
  brokerSockaddr(Addr, AddrLen);
  if (bind(ListenFd, reinterpret_cast<sockaddr *>(&Addr), AddrLen) != 0) {
    fprintf(stderr, "memory_delegation_broker: bind failed errno=%d\n", errno);
    close(ListenFd);
    return 1;
  }
  if (listen(ListenFd, 64) != 0) {
    fprintf(stderr, "memory_delegation_broker: listen failed errno=%d\n", errno);
    close(ListenFd);
    return 1;
  }

  signal(SIGTERM, onSignal);
  signal(SIGINT, onSignal);
  trace("ready num_cores=%u", NumCores);

  while (!StopRequested) {
    const int Client = accept4(ListenFd, nullptr, nullptr, SOCK_CLOEXEC);
    if (Client < 0) {
      if (errno == EINTR)
        continue;
      trace("accept failed errno=%d", errno);
      break;
    }

    BrokerRequest Request = {};
    if (!readExact(Client, &Request, sizeof(Request))) {
      close(Client);
      continue;
    }

    if (Request.Magic != kBrokerMagic || Request.CoreId >= NumCores) {
      trace("reject magic=0x%x core=%u", Request.Magic, Request.CoreId);
      (void)sendReply(Client, -1, -1);
      close(Client);
      continue;
    }

    const int ArenaFd = ArenaFds[Request.CoreId];
    trace("serve core=%u fd=%d", Request.CoreId, ArenaFd);
    (void)sendReply(Client, ArenaFd >= 0 ? 0 : -1, ArenaFd);
    close(Client);
  }

  trace("exiting stop_requested=%d", StopRequested ? 1 : 0);
  close(ListenFd);
  for (uint32_t I = 0; I < NumCores; ++I) {
    if (ArenaFds[I] >= 0)
      close(ArenaFds[I]);
  }
  return 0;
}
