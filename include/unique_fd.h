#ifndef UNIQUE_FD_H
#define UNIQUE_FD_H

#include <unistd.h>

namespace mtp {

class unique_fd {
private:
    int fd;

public:
    unique_fd() : fd(-1) {}
    explicit unique_fd(int fd) : fd(fd) {}

    ~unique_fd() {
        reset();
    }

    unique_fd(unique_fd&& other) noexcept : fd(other.fd) {
        other.fd = -1;
    }

    unique_fd& operator=(unique_fd&& other) noexcept {
        reset();
        fd = other.fd;
        other.fd = -1;
        return *this;
    }

    unique_fd(const unique_fd&) = delete;
    unique_fd& operator=(const unique_fd&) = delete;

    operator int() const { return fd; }

    bool valid() const { return fd >= 0; }

    bool operator==(int other) const { return fd == other; }

    void reset(int new_fd = -1) {
        if (fd >= 0) {
            ::close(fd);
        }
        fd = new_fd;
    }

    int release() {
        int tmp = fd;
        fd = -1;
        return tmp;
    }
};

} // namespace mtp

#endif // UNIQUE_FD_H
