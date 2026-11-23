// Requirements: UR-RES-7, UR-BLD-5
#pragma once

// The libtiff wrapper behind raster I/O (rulings S40, S41 and S52). Every handle is opened
// through TIFFClientOpenExt over our own std::FILE*, opened from the UTF-8 path through
// std::filesystem::u8path (_wfopen on Windows, with its default share mode, so a read
// handle can run beside a write handle). Each handle owns its own error collector: libtiff
// errors are appended to it, warnings go to logMessage at Debug, and nothing ever reaches
// libtiff's global handlers or stderr. There is no shared mutable state, so handles on
// different threads never interfere.

#include <slideio/processor/core/data_type.hpp>

#include <cstdint>
#include <memory>
#include <string>

struct tiff;
typedef struct tiff TIFF;

namespace slideio::processor::detail {

struct TiffHandleState;

// A move-only owner of one TIFF* and its error collector.
class TiffHandle {
public:
    TiffHandle() noexcept;
    TiffHandle(TiffHandle&& other) noexcept;
    TiffHandle& operator=(TiffHandle&& other) noexcept;
    TiffHandle(const TiffHandle&) = delete;
    TiffHandle& operator=(const TiffHandle&) = delete;
    // Closes the handle and swallows any failure.
    ~TiffHandle();

    // Creates or truncates the file at `utf8Path` as a BigTIFF (mode "w8"). Throws IoError
    // when the file cannot be opened or libtiff refuses it (the message names the raster
    // file by its role and its path), with the underlying text as the cause. libtiff is
    // given the path as the file's name, so its own text names the file too.
    static TiffHandle openWrite(const std::string& utf8Path);
    // Opens the TIFF at `utf8Path` for reading. Throws IoError naming the raster file by
    // its role and its path when the file cannot be opened or libtiff cannot read it, with
    // the underlying text as the cause.
    static TiffHandle openRead(const std::string& utf8Path);

    // The libtiff handle; null for a default-constructed, moved-from or closed handle.
    TIFF* get() const noexcept;

    // The path the handle was opened with (UTF-8); empty for a handle that holds nothing.
    const std::string& path() const noexcept;

    // Throws IoError "<operation>: libtiff failed on the raster file (<path>)", with the text
    // libtiff has reported since the last fail (if any) as the cause, and clears that text.
    // For a libtiff call on get() that returned failure.
    [[noreturn]] void fail(const char* operation) const;

    // Pushes what libtiff has written so far out of the stdio buffer to the OS, so another
    // handle on the same file sees it (the pyramid read-back opens one after each
    // TIFFWriteDirectory). Throws IoError naming the file by its path when the flush
    // fails. A no-op on a handle that holds nothing.
    void flush();

    // Flushes the TIFF (a write handle writes its pending directory), closes it, then
    // closes the file. Throws IoError naming the file by its path when either step fails;
    // the handle is released either way. A no-op on a handle that holds nothing.
    void close();

private:
    explicit TiffHandle(std::unique_ptr<TiffHandleState> state) noexcept;
    std::unique_ptr<TiffHandleState> state_;
};

// The TIFF SampleFormat of `type` (ruling S41): SAMPLEFORMAT_UINT for Byte, UInt16, UInt32
// and UInt64; SAMPLEFORMAT_INT for Int8, Int16, Int32 and Int64; SAMPLEFORMAT_IEEEFP for
// Float16, Float32 and Float64. Throws InternalError for Unknown or an out-of-range value,
// which a declaration never carries.
uint16_t tiffSampleFormat(DataType type);

// The TIFF BitsPerSample of `type`: its size in bits. Throws InternalError for Unknown or
// an out-of-range value.
uint16_t tiffBitsPerSample(DataType type);

} // namespace slideio::processor::detail
