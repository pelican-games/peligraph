#include "outputfile.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace Pelican {
namespace {

std::string frameFileName(std::string_view name, std::uint64_t frame) {
    std::ostringstream out;
    out << name << '_' << std::setw(6) << std::setfill('0') << frame << ".rgba";
    return out.str();
}

std::vector<std::uint8_t> encodedFile(const OutputFileImage &image) {
    const auto expected = static_cast<std::uint64_t>(image.width) * image.height * 4;
    if (image.pixels.size() != expected) return {};
    std::vector<std::uint8_t> bytes;
    bytes.reserve(output_file_header_bytes + image.pixels.size());
    bytes.push_back('P');
    bytes.push_back('V');
    bytes.push_back('O');
    bytes.push_back('1');
    const auto put = [&](std::uint32_t value) {
        bytes.push_back(static_cast<std::uint8_t>(value & 0xffu));
        bytes.push_back(static_cast<std::uint8_t>((value >> 8) & 0xffu));
        bytes.push_back(static_cast<std::uint8_t>((value >> 16) & 0xffu));
        bytes.push_back(static_cast<std::uint8_t>((value >> 24) & 0xffu));
    };
    put(image.width);
    put(image.height);
    put(static_cast<std::uint32_t>(image.format));
    bytes.insert(bytes.end(), image.pixels.begin(), image.pixels.end());
    return bytes;
}

#ifdef _WIN32

OutputFileWriteResult writeWindows(const std::filesystem::path &directory,
                                   const std::string &final_name,
                                   const std::string &partial_name,
                                   const std::vector<std::uint8_t> &bytes) {
    const auto final_path = directory / final_name;
    const auto partial_path = directory / partial_name;
    if (GetFileAttributesW(final_path.c_str()) != INVALID_FILE_ATTRIBUTES) {
        return {OutputFileWriteStatus::exists, "output.exists@1: " + final_path.string()};
    }
    HANDLE file = CreateFileW(partial_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        if (GetLastError() == ERROR_FILE_EXISTS) {
            return {OutputFileWriteStatus::failed,
                    "output.write_failed@1: partial file already exists: " + partial_path.string()};
        }
        return {OutputFileWriteStatus::failed, "output.write_failed@1: cannot create " +
                                                   partial_path.string()};
    }
    std::size_t offset = 0;
    bool ok = true;
    while (offset < bytes.size()) {
        const auto chunk = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - offset, 1u << 30));
        DWORD written = 0;
        if (!WriteFile(file, bytes.data() + offset, chunk, &written, nullptr) || written == 0) {
            ok = false;
            break;
        }
        offset += written;
    }
    if (!CloseHandle(file) && ok) ok = false;
    if (!ok) {
        DeleteFileW(partial_path.c_str());
        return {OutputFileWriteStatus::failed,
                "output.write_failed@1: cannot write " + partial_path.string()};
    }
    if (!MoveFileExW(partial_path.c_str(), final_path.c_str(), 0)) {
        const auto error = GetLastError();
        DeleteFileW(partial_path.c_str());
        if (error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS) {
            return {OutputFileWriteStatus::exists, "output.exists@1: " + final_path.string()};
        }
        return {OutputFileWriteStatus::failed,
                "output.write_failed@1: cannot publish " + final_path.string()};
    }
    return {OutputFileWriteStatus::written, {}};
}

#else

OutputFileWriteResult writePosix(const std::filesystem::path &directory,
                                 const std::string &final_name,
                                 const std::string &partial_name,
                                 const std::vector<std::uint8_t> &bytes,
                                 const std::filesystem::path &final_path,
                                 const std::filesystem::path &partial_path) {
    if (::access(final_path.c_str(), F_OK) == 0) {
        return {OutputFileWriteStatus::exists, "output.exists@1: " + final_path.string()};
    }
    const int fd = ::open(partial_path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
        if (errno == EEXIST) {
            return {OutputFileWriteStatus::failed,
                    "output.write_failed@1: partial file already exists: " + partial_path.string()};
        }
        return {OutputFileWriteStatus::failed,
                "output.write_failed@1: cannot create " + partial_path.string()};
    }
    std::size_t offset = 0;
    bool ok = true;
    while (offset < bytes.size()) {
        const auto written = ::write(fd, bytes.data() + offset, bytes.size() - offset);
        if (written <= 0) {
            if (written < 0 && errno == EINTR) continue;
            ok = false;
            break;
        }
        offset += static_cast<std::size_t>(written);
    }
    if (::close(fd) != 0 && ok) ok = false;
    if (!ok) {
        ::unlink(partial_path.c_str());
        return {OutputFileWriteStatus::failed,
                "output.write_failed@1: cannot write " + partial_path.string()};
    }
    // link() publishes the complete inode without ever replacing an existing
    // final name. std::filesystem::rename is deliberately not used.
    if (::link(partial_path.c_str(), final_path.c_str()) != 0) {
        const auto error = errno;
        ::unlink(partial_path.c_str());
        if (error == EEXIST) {
            return {OutputFileWriteStatus::exists, "output.exists@1: " + final_path.string()};
        }
        return {OutputFileWriteStatus::failed,
                "output.write_failed@1: cannot publish " + final_path.string()};
    }
    ::unlink(partial_path.c_str());
    (void)directory;
    (void)final_name;
    (void)partial_name;
    return {OutputFileWriteStatus::written, {}};
}

#endif

} // namespace

OutputFileWriteResult writeOutputFrameFile(const std::filesystem::path &directory,
                                           std::string_view name,
                                           std::uint64_t frame,
                                           const OutputFileImage &image) {
    if (image.width == 0 || image.height == 0 || image.pixels.size() !=
            static_cast<std::uint64_t>(image.width) * image.height * 4) {
        return {OutputFileWriteStatus::failed,
                "output.write_failed@1: pixel buffer does not match the image"};
    }
    const auto final_name = frameFileName(name, frame);
    const auto partial_name = final_name + ".partial";
    auto bytes = encodedFile(image);
    if (bytes.empty()) {
        return {OutputFileWriteStatus::failed,
                "output.write_failed@1: cannot encode the image"};
    }
#ifdef _WIN32
    return writeWindows(directory, final_name, partial_name, bytes);
#else
    const auto final_path = directory / final_name;
    const auto partial_path = directory / partial_name;
    return writePosix(directory, final_name, partial_name, bytes, final_path, partial_path);
#endif
}

} // namespace Pelican
