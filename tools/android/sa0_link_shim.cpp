// SA0 diagnostic only: Android's SELinux policy refuses link(2) (EACCES) for
// the shell and app domains. Linked with -Wl,--wrap=link into a copy of a test
// executable, this substitutes an exclusive create + copy for a refused hard
// link, to show which failures the missing hard link alone explains. Not a
// fix: a copy is not the atomic publication the transaction relies on.
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

extern "C" int __real_link(const char *from, const char *to);

extern "C" int __wrap_link(const char *from, const char *to) {
    if (__real_link(from, to) == 0) return 0;
    if (errno != EACCES && errno != EPERM) return -1;
    const int in = open(from, O_RDONLY | O_CLOEXEC);
    if (in < 0) return -1;
    const int out = open(to, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (out < 0) {
        const int saved = errno;
        close(in);
        errno = saved;
        return -1;
    }
    char buffer[65536];
    for (;;) {
        const ssize_t n = read(in, buffer, sizeof buffer);
        if (n == 0) break;
        if (n < 0 || write(out, buffer, static_cast<size_t>(n)) != n) {
            close(in);
            close(out);
            unlink(to);
            errno = EIO;
            return -1;
        }
    }
    fsync(out);
    close(in);
    close(out);
    return 0;
}
