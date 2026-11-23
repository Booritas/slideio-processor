#include "storage/detail/tiff_io.hpp"

#include "core/detail/diagnostics.hpp"
#include "storage/detail/file_io.hpp"

#include <slideio/processor/core/error.hpp>
#include <slideio/processor/core/log.hpp>

#include <tiffio.h>

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <share.h>
#endif

namespace slideio::processor::detail {

// One handle's file, libtiff handle and error collector (declared in the header). libtiff keeps a pointer to it as
// the handlers' user data and as the client data, so it lives on the heap and never moves.
struct TiffHandleState {
    // The last transfer on `file` since its last seek, for the direction-switch guard.
    enum class Direction { None, Read, Write };

    bool write = false;
    // The file's path (UTF-8). libtiff is given it as the file's name, so it prefixes
    // libtiff's own messages, and every error of this handle names it.
    std::string path;
    std::FILE* file = nullptr;
    Direction direction = Direction::None;
    TIFF* tif = nullptr;
    std::string errors; // libtiff errors since the last fail, "; "-separated
};

namespace {

// The message libtiff formatted, or "" when formatting fails.
std::string formatMessage(const char* module, const char* fmt, va_list ap) {
    va_list copy;
    va_copy(copy, ap);
    const int length = std::vsnprintf(nullptr, 0, fmt, copy);
    va_end(copy);
    if (length < 0) return std::string();
    std::vector<char> text(static_cast<size_t>(length) + 1);
    std::vsnprintf(text.data(), text.size(), fmt, ap);
    std::string message;
    if (module != nullptr && *module != '\0') message = std::string(module) + ": ";
    message.append(text.data(), static_cast<size_t>(length));
    return message;
}

// Returning 1 stops libtiff from calling its global handlers as well.
int collectError(TIFF*, void* userData, const char* module, const char* fmt, va_list ap) {
    try {
        auto* state = static_cast<TiffHandleState*>(userData);
        if (state == nullptr) return 1;
        const std::string message = formatMessage(module, fmt, ap);
        if (!state->errors.empty()) state->errors += "; ";
        state->errors += message;
    } catch (...) {
        // Out of memory while collecting: the failing call still reports failure.
    }
    return 1;
}

int logWarning(TIFF*, void* userData, const char* module, const char* fmt, va_list ap) {
    try {
        const std::string message = formatMessage(module, fmt, ap);
        const auto* state = static_cast<const TiffHandleState*>(userData);
        const std::string lead = state != nullptr ? withPath("libtiff warning on a raster file", state->path)
                                                  : std::string("libtiff warning on a raster file");
        logMessage(LogLevel::Debug, "%s: %s", lead.c_str(), message.c_str());
    } catch (...) {
    }
    return 1;
}

// Our private libtiff's global handlers print to stderr; silence them once. They cannot
// reach SlideIO's copy of libtiff.
void silenceGlobalHandlers() {
    static std::once_flag once;
    std::call_once(once, [] {
        TIFFSetErrorHandler(nullptr);
        TIFFSetWarningHandler(nullptr);
    });
}

// The handle's file, ready for a transfer in `direction`. ISO C requires a file
// positioning call between output and input on an update stream ("w+b"), so a switch of
// direction with no seek in between gets a seek to the current position. libtiff seeks
// before nearly every transfer, and no current test fails without this guard. Returns
// null when that seek fails, with the reason added to the handle's errors; the transfer
// then reports failure, and libtiff reports it in turn.
std::FILE* fileFor(thandle_t client, TiffHandleState::Direction direction) {
    auto* state = static_cast<TiffHandleState*>(client);
    if (state->direction != direction && state->direction != TiffHandleState::Direction::None) {
        if (std::fseek(state->file, 0, SEEK_CUR) != 0) {
            const int error = errno;
            try {
                if (!state->errors.empty()) state->errors += "; ";
                state->errors += "seek on a switch between reading and writing: " + errnoText(error);
            } catch (...) {
                // Out of memory while collecting: the transfer still reports failure.
            }
            return nullptr;
        }
    }
    state->direction = direction;
    return state->file;
}

tmsize_t readProc(thandle_t client, void* buffer, tmsize_t size) {
    if (size <= 0) return 0;
    std::FILE* file = fileFor(client, TiffHandleState::Direction::Read);
    if (file == nullptr) return static_cast<tmsize_t>(-1);
    return static_cast<tmsize_t>(std::fread(buffer, 1, static_cast<size_t>(size), file));
}

tmsize_t writeProc(thandle_t client, void* buffer, tmsize_t size) {
    if (size <= 0) return 0;
    std::FILE* file = fileFor(client, TiffHandleState::Direction::Write);
    if (file == nullptr) return static_cast<tmsize_t>(-1);
    return static_cast<tmsize_t>(std::fwrite(buffer, 1, static_cast<size_t>(size), file));
}

toff_t seekProc(thandle_t client, toff_t offset, int whence) {
    auto* state = static_cast<TiffHandleState*>(client);
    std::FILE* file = state->file;
    state->direction = TiffHandleState::Direction::None;
    if (seekFile(file, static_cast<int64_t>(offset), whence) != 0) return static_cast<toff_t>(-1);
    const int64_t position = tellFile(file);
    return position < 0 ? static_cast<toff_t>(-1) : static_cast<toff_t>(position);
}

// TiffHandle::close closes the file itself, after libtiff, so it can report the failure.
int closeProc(thandle_t) { return 0; }

toff_t sizeProc(thandle_t client) {
    auto* state = static_cast<TiffHandleState*>(client);
    std::FILE* file = state->file;
    state->direction = TiffHandleState::Direction::None; // it seeks
    const int64_t position = tellFile(file);
    if (position < 0 || seekFile(file, 0, SEEK_END) != 0) return 0;
    const int64_t size = tellFile(file);
    seekFile(file, position, SEEK_SET);
    return size < 0 ? 0 : static_cast<toff_t>(size);
}

// No memory mapping: libtiff reads through readProc.
int mapProc(thandle_t, void**, toff_t*) { return 0; }
void unmapProc(thandle_t, void*, toff_t) {}

// Opens the file through u8path, so non-ASCII paths work on every platform (C-4). On
// Windows _wfsopen with _SH_DENYNO is _wfopen's default share mode, spelled out: it lets
// readers and a writer open the same file at once. (_wfopen_s would deny sharing.)
std::FILE* openFile(const std::string& utf8Path, bool write) {
    const std::filesystem::path path = std::filesystem::u8path(utf8Path);
#ifdef _WIN32
    return _wfsopen(path.c_str(), write ? L"w+b" : L"rb", _SH_DENYNO);
#else
    // Unverified here: the POSIX side is compiled on Linux and macOS only.
    return std::fopen(path.c_str(), write ? "w+b" : "rb");
#endif
}

std::unique_ptr<TiffHandleState> openHandle(const std::string& utf8Path, const char* mode, bool write) {
    silenceGlobalHandlers();
    auto state = std::make_unique<TiffHandleState>();
    state->write = write;
    state->path = utf8Path;
    errno = 0;
    state->file = openFile(utf8Path, write);
    if (state->file == nullptr) {
        const int error = errno;
        throwIo(withPath("TIFFOpen: cannot open the raster file", utf8Path),
                error != 0 ? errnoText(error) : std::string("the file cannot be opened"));
    }

    TIFFOpenOptions* options = TIFFOpenOptionsAlloc();
    if (options == nullptr) {
        std::fclose(state->file);
        throw std::bad_alloc();
    }
    TIFFOpenOptionsSetErrorHandlerExtR(options, collectError, state.get());
    TIFFOpenOptionsSetWarningHandlerExtR(options, logWarning, state.get());
    // libtiff copies the name; it is the path, so libtiff's own text names the file.
    state->tif = TIFFClientOpenExt(state->path.c_str(), mode, state.get(), readProc, writeProc, seekProc, closeProc,
                                   sizeProc, mapProc, unmapProc, options);
    TIFFOpenOptionsFree(options);
    if (state->tif == nullptr) {
        std::fclose(state->file);
        throwIo(withPath("TIFFClientOpen: libtiff failed on the raster file", utf8Path), state->errors);
    }
    return state;
}

} // namespace

TiffHandle::TiffHandle() noexcept = default;
TiffHandle::TiffHandle(std::unique_ptr<TiffHandleState> state) noexcept : state_(std::move(state)) {}
TiffHandle::TiffHandle(TiffHandle&& other) noexcept = default;

TiffHandle& TiffHandle::operator=(TiffHandle&& other) noexcept {
    if (this != &other) {
        try {
            close();
        } catch (...) {
        }
        state_ = std::move(other.state_);
    }
    return *this;
}

TiffHandle::~TiffHandle() {
    try {
        close();
    } catch (...) {
    }
}

TiffHandle TiffHandle::openWrite(const std::string& utf8Path) {
    return TiffHandle(openHandle(utf8Path, "w8", true));
}

TiffHandle TiffHandle::openRead(const std::string& utf8Path) {
    return TiffHandle(openHandle(utf8Path, "r", false));
}

TIFF* TiffHandle::get() const noexcept { return state_ ? state_->tif : nullptr; }

const std::string& TiffHandle::path() const noexcept {
    static const std::string kNone;
    return state_ ? state_->path : kNone;
}

void TiffHandle::fail(const char* operation) const {
    std::string cause;
    if (state_) cause.swap(state_->errors);
    std::string message = std::string(operation) + ": libtiff failed on the raster file";
    if (state_) message = withPath(message, state_->path);
    throwIo(message, cause);
}

void TiffHandle::flush() {
    if (!state_ || state_->file == nullptr) return;
    errno = 0;
    if (std::fflush(state_->file) != 0) {
        const int error = errno != 0 ? errno : EIO;
        throwIo(withPath("fflush: cannot flush the raster file", state_->path), errnoText(error));
    }
}

void TiffHandle::close() {
    if (!state_) return;
    std::unique_ptr<TiffHandleState> state = std::move(state_);
    std::string failure; // the message, or "" when every step succeeded
    std::string cause;
    if (state->tif != nullptr) {
        // TIFFFlush writes a write handle's pending directory; TIFFClose reports nothing.
        if (state->write && TIFFFlush(state->tif) == 0) {
            failure = withPath("TIFFFlush: libtiff failed on the raster file", state->path);
            cause = state->errors;
        }
        TIFFClose(state->tif);
        state->tif = nullptr;
    }
    if (state->file != nullptr) {
        errno = 0;
        int error = std::fflush(state->file) == 0 ? 0 : errno;
        if (std::fclose(state->file) != 0 && error == 0) error = errno != 0 ? errno : EIO;
        state->file = nullptr;
        if (error != 0 && failure.empty()) {
            failure = withPath("fclose: cannot close the raster file", state->path);
            cause = errnoText(error);
        }
    }
    if (!failure.empty()) throwIo(failure, cause);
}

uint16_t tiffSampleFormat(DataType type) {
    switch (type) {
    case DataType::Byte:
    case DataType::UInt16:
    case DataType::UInt32:
    case DataType::UInt64:
        return SAMPLEFORMAT_UINT;
    case DataType::Int8:
    case DataType::Int16:
    case DataType::Int32:
    case DataType::Int64:
        return SAMPLEFORMAT_INT;
    case DataType::Float16:
    case DataType::Float32:
    case DataType::Float64:
        return SAMPLEFORMAT_IEEEFP;
    default:
        throw InternalError("tiffSampleFormat: not a raster sample type");
    }
}

uint16_t tiffBitsPerSample(DataType type) {
    const int32_t bytes = dataTypeSizeBytes(type);
    if (bytes <= 0) throw InternalError("tiffBitsPerSample: not a raster sample type");
    return static_cast<uint16_t>(bytes * 8);
}

} // namespace slideio::processor::detail
