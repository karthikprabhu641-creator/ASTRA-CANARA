#include "kvdb/storage/file.hpp"
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <filesystem>
#include <cstdlib>
#if defined(KVDB_ENABLE_CRASH_HOOKS)
#include "crash_hook.hpp"
#endif

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#endif

namespace kvdb::storage {

struct File::Impl {
    std::string path;
    std::uint64_t file_size{0};
#if defined(_WIN32)
    HANDLE handle{INVALID_HANDLE_VALUE};
#else
    int fd{-1};
#endif
};

File::File() : impl_(std::make_unique<Impl>()) {}

File::~File() {
    close();
}

File::File(File&& other) noexcept = default;
File& File::operator=(File&& other) noexcept = default;

Status File::open(const std::string& path, bool create_if_missing) {
    const Status close_status = close();
    if (!is_ok(close_status)) return close_status;
    impl_->path = path;

#if defined(_WIN32)
    // Convert UTF-8 path to UTF-16 wide string for full Unicode support
    int wlen = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    std::wstring wpath(wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, &wpath[0], wlen);

    DWORD creation = create_if_missing ? OPEN_ALWAYS : OPEN_EXISTING;
    HANDLE h = CreateFileW(
        wpath.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        creation,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );

    if (h == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND) return Status::NotFound;
        if (err == ERROR_DISK_FULL || err == ERROR_HANDLE_DISK_FULL) return Status::NoSpace;
        return Status::IoError;
    }

    LARGE_INTEGER size_li;
    if (!GetFileSizeEx(h, &size_li)) {
        CloseHandle(h);
        return Status::IoError;
    }

    impl_->handle = h;
    impl_->file_size = static_cast<std::uint64_t>(size_li.QuadPart);
    return Status::Ok;
#else
    int flags = O_RDWR;
    if (create_if_missing) {
        flags |= O_CREAT;
    }
    int fd = ::open(path.c_str(), flags, 0644);
    if (fd < 0) {
        if (errno == ENOENT) return Status::NotFound;
        if (errno == ENOSPC) return Status::NoSpace;
        return Status::IoError;
    }

    struct stat st;
    if (::fstat(fd, &st) != 0) {
        ::close(fd);
        return Status::IoError;
    }

    impl_->fd = fd;
    impl_->file_size = static_cast<std::uint64_t>(st.st_size);
    return Status::Ok;
#endif
}

Status File::close() {
#if defined(_WIN32)
    if (impl_->handle != INVALID_HANDLE_VALUE) {
        const BOOL closed = CloseHandle(impl_->handle);
        impl_->handle = INVALID_HANDLE_VALUE;
        impl_->file_size = 0;
        if (!closed) return Status::IoError;
    }
#else
    if (impl_->fd >= 0) {
        const int fd = impl_->fd;
        impl_->fd = -1;
        impl_->file_size = 0;
        if (::close(fd) != 0) return Status::IoError;
    }
#endif
    return Status::Ok;
}

Status File::append(const std::uint8_t* data, std::size_t size) {
    if (!is_open()) return Status::NotOpen;
    if (size == 0) return Status::Ok;

#if defined(_WIN32)
#if defined(KVDB_ENABLE_CRASH_HOOKS)
    bool partial_phase_reported = false;
    const bool report_phases = std::getenv("KVDB_TEST_REPORT_PHASES") != nullptr;
#endif
    std::size_t total_written = 0;
    while (total_written < size) {
        std::size_t max_write_size = 64U * 1024U * 1024U;
#if defined(KVDB_ENABLE_CRASH_HOOKS)
        if (report_phases) max_write_size = 64U * 1024U;
#endif
        DWORD to_write = static_cast<DWORD>(
            std::min<std::size_t>(size - total_written, max_write_size));
        DWORD written = 0;

        OVERLAPPED ov{};
        std::uint64_t offset = impl_->file_size + total_written;
        ov.Offset = static_cast<DWORD>(offset & 0xFFFFFFFF);
        ov.OffsetHigh = static_cast<DWORD>((offset >> 32) & 0xFFFFFFFF);

        if (!WriteFile(impl_->handle, data + total_written, to_write, &written, &ov)) {
            DWORD err = GetLastError();
            if (err == ERROR_DISK_FULL || err == ERROR_HANDLE_DISK_FULL) {
                return Status::NoSpace;
            }
            return Status::IoError;
        }

        if (written == 0) {
            return Status::IoError;
        }
        total_written += written;
        impl_->file_size += written;
#if defined(KVDB_ENABLE_CRASH_HOOKS)
        if (report_phases && !partial_phase_reported && total_written < size) {
            torture::report_test_phase("file_append_partial_write");
            partial_phase_reported = true;
        }
#endif
    }
    return Status::Ok;
#else
#if defined(KVDB_ENABLE_CRASH_HOOKS)
    bool partial_phase_reported = false;
    const bool report_phases = std::getenv("KVDB_TEST_REPORT_PHASES") != nullptr;
#endif
    std::size_t total_written = 0;
    while (total_written < size) {
        std::size_t write_size = size - total_written;
#if defined(KVDB_ENABLE_CRASH_HOOKS)
        if (report_phases) write_size = std::min<std::size_t>(write_size, 64U * 1024U);
#endif
        ssize_t written = ::pwrite(
            impl_->fd,
            data + total_written,
            write_size,
            static_cast<off_t>(impl_->file_size + total_written)
        );
        if (written < 0) {
            if (errno == EINTR) continue;
            if (errno == ENOSPC) return Status::NoSpace;
            return Status::IoError;
        }
        if (written == 0) return Status::IoError;
        total_written += static_cast<std::size_t>(written);
        impl_->file_size += static_cast<std::uint64_t>(written);
#if defined(KVDB_ENABLE_CRASH_HOOKS)
        if (report_phases && !partial_phase_reported && total_written < size) {
            torture::report_test_phase("file_append_partial_write");
            partial_phase_reported = true;
        }
#endif
    }
    return Status::Ok;
#endif
}

Status File::read_at(std::uint64_t offset, std::uint8_t* buffer, std::size_t size) const {
    if (!is_open()) return Status::NotOpen;
    if (size == 0) return Status::Ok;
    if (offset + size > impl_->file_size) {
        return Status::IoError; // Attempt to read past EOF
    }

#if defined(_WIN32)
    std::size_t total_read = 0;
    while (total_read < size) {
        DWORD to_read = static_cast<DWORD>(std::min<std::size_t>(size - total_read, 64 * 1024 * 1024));
        DWORD bytes_read = 0;

        OVERLAPPED ov{};
        std::uint64_t cur_offset = offset + total_read;
        ov.Offset = static_cast<DWORD>(cur_offset & 0xFFFFFFFF);
        ov.OffsetHigh = static_cast<DWORD>((cur_offset >> 32) & 0xFFFFFFFF);

        if (!ReadFile(impl_->handle, buffer + total_read, to_read, &bytes_read, &ov)) {
            return Status::IoError;
        }
        if (bytes_read == 0) {
            return Status::IoError;
        }
        total_read += bytes_read;
    }
    return Status::Ok;
#else
    std::size_t total_read = 0;
    while (total_read < size) {
        ssize_t bytes_read = ::pread(
            impl_->fd,
            buffer + total_read,
            size - total_read,
            static_cast<off_t>(offset + total_read)
        );
        if (bytes_read < 0) {
            if (errno == EINTR) continue;
            return Status::IoError;
        }
        if (bytes_read == 0) return Status::IoError;
        total_read += static_cast<std::size_t>(bytes_read);
    }
    return Status::Ok;
#endif
}

Status File::sync() {
    if (!is_open()) return Status::NotOpen;

#if defined(_WIN32)
    if (!FlushFileBuffers(impl_->handle)) {
        return Status::IoError;
    }
    return Status::Ok;
#else
    #if defined(__APPLE__)
        if (::fcntl(impl_->fd, F_FULLFSYNC) != 0) return Status::IoError;
    #else
        if (::fdatasync(impl_->fd) != 0) {
            if (::fsync(impl_->fd) != 0) return Status::IoError;
        }
    #endif
    return Status::Ok;
#endif
}

Status File::truncate(std::uint64_t new_size) {
    if (!is_open()) return Status::NotOpen;

#if defined(_WIN32)
    LARGE_INTEGER li;
    li.QuadPart = static_cast<LONGLONG>(new_size);
    if (!SetFilePointerEx(impl_->handle, li, nullptr, FILE_BEGIN)) {
        return Status::IoError;
    }
    if (!SetEndOfFile(impl_->handle)) {
        return Status::IoError;
    }
    impl_->file_size = new_size;
#if defined(KVDB_ENABLE_CRASH_HOOKS)
    torture::report_test_phase("file_truncate_after_size_change");
#endif
    return sync();
#else
    if (::ftruncate(impl_->fd, static_cast<off_t>(new_size)) != 0) {
        return Status::IoError;
    }
    impl_->file_size = new_size;
#if defined(KVDB_ENABLE_CRASH_HOOKS)
    torture::report_test_phase("file_truncate_after_size_change");
#endif
    return sync();
#endif
}

std::uint64_t File::size() const {
    return impl_->file_size;
}

bool File::is_open() const {
#if defined(_WIN32)
    return impl_->handle != INVALID_HANDLE_VALUE;
#else
    return impl_->fd >= 0;
#endif
}

const std::string& File::path() const {
    return impl_->path;
}

Status atomic_rename_file(const std::string& old_path, const std::string& new_path) {
#if defined(_WIN32)
    int old_wlen = MultiByteToWideChar(CP_UTF8, 0, old_path.c_str(), -1, nullptr, 0);
    std::wstring old_w(old_wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, old_path.c_str(), -1, &old_w[0], old_wlen);

    int new_wlen = MultiByteToWideChar(CP_UTF8, 0, new_path.c_str(), -1, nullptr, 0);
    std::wstring new_w(new_wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, new_path.c_str(), -1, &new_w[0], new_wlen);

    if (!MoveFileExW(old_w.c_str(), new_w.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return Status::IoError;
    }
    return Status::Ok;
#else
    if (::rename(old_path.c_str(), new_path.c_str()) != 0) {
        return Status::IoError;
    }
    return Status::Ok;
#endif
}

} // namespace kvdb::storage
