/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-CLA-applies
 *
 * MuseScore Studio
 * Music Composition & Notation
 *
 * Copyright (C) 2021 MuseScore Limited and others
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
#include "zipcontainer.h"

#include <algorithm>
#include <cstddef>
#include <ctime>
#include <cstring>
#include <zlib.h>

#include "global/io/dir.h"

#include "log.h"

// Zip standard version for archives handled by this API
// (actually, the only basic support of this version is implemented but it is enough for now)
#define ZIP_VERSION 20

#define ZDEBUG if (0) LOGD

using namespace muse::io;

typedef unsigned long int ulong;
typedef unsigned short int ushort;
typedef unsigned int uint;

namespace muse {
static inline uint32_t readUInt(const uint8_t* data)
{
    return uint32_t(data[0]) | (uint32_t(data[1]) << 8) | (uint32_t(data[2]) << 16) | (uint32_t(data[3]) << 24);
}

static inline ushort readUShort(const uint8_t* data)
{
    return ushort(data[0]) | ushort(ushort(data[1]) << 8);
}

static inline void writeUInt(uint8_t* data, uint i)
{
    data[0] = i & 0xff;
    data[1] = (i >> 8) & 0xff;
    data[2] = (i >> 16) & 0xff;
    data[3] = (i >> 24) & 0xff;
}

static inline void writeUShort(uint8_t* data, ushort i)
{
    data[0] = i & 0xff;
    data[1] = (i >> 8) & 0xff;
}

static inline void copyUInt(uint8_t* dest, const uint8_t* src)
{
    dest[0] = src[0];
    dest[1] = src[1];
    dest[2] = src[2];
    dest[3] = src[3];
}

static inline void copyUShort(uint8_t* dest, const uint8_t* src)
{
    dest[0] = src[0];
    dest[1] = src[1];
}

static void writeMSDosDate(uint8_t* dest, const std::tm& dt)
{
    if (dt.tm_year > 0) {
        uint16_t time
            =(dt.tm_hour << 11)     // 5 bit hour
              | (dt.tm_min << 5)    // 6 bit minute
              | (dt.tm_sec >> 1);   // 5 bit double seconds

        dest[0] = time & 0xff;
        dest[1] = time >> 8;

        uint16_t date
            =((dt.tm_year + 1900 - 1980) << 9) // 7 bit year 1980-based
              | ((dt.tm_mon + 1) << 5)         // 4 bit month
              | (dt.tm_mday);                  // 5 bit day

        dest[2] = char(date);
        dest[3] = char(date >> 8);
    } else {
        dest[0] = 0;
        dest[1] = 0;
        dest[2] = 0;
        dest[3] = 0;
    }
}

static int inflate(Bytef* dest, ulong* destLen, const Bytef* source, ulong sourceLen)
{
    z_stream stream {};
    int err;

    stream.next_in = const_cast<Bytef*>(source);
    stream.avail_in = (uInt)sourceLen;
    if ((uLong)stream.avail_in != sourceLen) {
        return Z_BUF_ERROR;
    }

    stream.next_out = dest;
    stream.avail_out = (uInt) * destLen;
    if ((uLong)stream.avail_out != *destLen) {
        return Z_BUF_ERROR;
    }

    stream.zalloc = (alloc_func)0;
    stream.zfree = (free_func)0;

    err = inflateInit2(&stream, -MAX_WBITS);
    if (err != Z_OK) {
        return err;
    }

    err = inflate(&stream, Z_FINISH);
    if (err != Z_STREAM_END) {
        inflateEnd(&stream);
        if (err == Z_NEED_DICT || (err == Z_BUF_ERROR && stream.avail_in == 0)) {
            return Z_DATA_ERROR;
        }
        return err == Z_OK ? Z_BUF_ERROR : err;
    }

    // The declared compressed size is part of the entry; trailing bytes are corrupt.
    if (stream.avail_in != 0) {
        inflateEnd(&stream);
        return Z_DATA_ERROR;
    }

    *destLen = stream.total_out;

    err = inflateEnd(&stream);
    return err;
}

static int deflate(Bytef* dest, ulong* destLen, const Bytef* source, ulong sourceLen)
{
    z_stream stream;
    int err;

    stream.next_in = const_cast<Bytef*>(source);
    stream.avail_in = (uInt)sourceLen;
    stream.next_out = dest;
    stream.avail_out = (uInt) * destLen;
    if ((uLong)stream.avail_out != *destLen) {
        return Z_BUF_ERROR;
    }

    stream.zalloc = (alloc_func)0;
    stream.zfree = (free_func)0;
    stream.opaque = (voidpf)0;

    err = deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
    if (err != Z_OK) {
        return err;
    }

    err = deflate(&stream, Z_FINISH);
    if (err != Z_STREAM_END) {
        deflateEnd(&stream);
        return err == Z_OK ? Z_BUF_ERROR : err;
    }
    *destLen = stream.total_out;

    err = deflateEnd(&stream);
    return err;
}

namespace WindowsFileAttributes {
enum {
    Dir        = 0x10, // FILE_ATTRIBUTE_DIRECTORY
    File       = 0x80, // FILE_ATTRIBUTE_NORMAL
    TypeMask   = 0x90,

    ReadOnly   = 0x01, // FILE_ATTRIBUTE_READONLY
    PermMask   = 0x01
};
}

namespace UnixFileAttributes {
enum {
    Dir        = 0040000, // __S_IFDIR
    File       = 0100000, // __S_IFREG
    SymLink    = 0120000, // __S_IFLNK
    TypeMask   = 0170000, // __S_IFMT

    ReadUser   = 0400, // __S_IRUSR
    WriteUser  = 0200, // __S_IWUSR
    ExeUser    = 0100, // __S_IXUSR
    ReadGroup  = 0040, // __S_IRGRP
    WriteGroup = 0020, // __S_IWGRP
    ExeGroup   = 0010, // __S_IXGRP
    ReadOther  = 0004, // __S_IROTH
    WriteOther = 0002, // __S_IWOTH
    ExeOther   = 0001, // __S_IXOTH
    PermMask   = 0777
};
}

static std::tm readMSDosDate(const uint8_t* src)
{
    std::tm tm;
    uint dosDate = readUInt(src);
    uint64_t uDate;
    uDate = (uint64_t)(dosDate >> 16);
    tm.tm_mday = (uDate & 0x1f);
    tm.tm_mon = ((uDate & 0x1E0) >> 5) - 1;
    tm.tm_year = (((uDate & 0x0FE00) >> 9) + 1980 - 1900);
    tm.tm_hour = ((dosDate & 0xF800) >> 11);
    tm.tm_min =  ((dosDate & 0x7E0) >> 5);
    tm.tm_sec =  ((dosDate & 0x1f) << 1);

    return tm;
}

// for details, see http://www.pkware.com/documents/casestudies/APPNOTE.TXT

enum HostOS {
    HostFAT      = 0,
    HostAMIGA    = 1,
    HostVMS      = 2,  // VAX/VMS
    HostUnix     = 3,
    HostVM_CMS   = 4,
    HostAtari    = 5,  // what if it's a minix filesystem? [cjh]
    HostHPFS     = 6,  // filesystem used by OS/2 (and NT 3.x)
    HostMac      = 7,
    HostZ_System = 8,
    HostCPM      = 9,
    HostTOPS20   = 10, // pkzip 2.50 NTFS
    HostNTFS     = 11, // filesystem used by Windows NT
    HostQDOS     = 12, // SMS/QDOS
    HostAcorn    = 13, // Archimedes Acorn RISC OS
    HostVFAT     = 14, // filesystem used by Windows 95, NT
    HostMVS      = 15,
    HostBeOS     = 16, // hybrid POSIX/database filesystem
    HostTandem   = 17,
    HostOS400    = 18,
    HostOSX      = 19
};

enum GeneralPurposeFlag {
    Encrypted = 0x01,
    AlgTune1 = 0x02,
    AlgTune2 = 0x04,
    HasDataDescriptor = 0x08,
    PatchedData = 0x20,
    StrongEncrypted = 0x40,
    Utf8Names = 0x0800,
    CentralDirectoryEncrypted = 0x2000
};

enum CompressionMethod {
    CompressionMethodStored = 0,
    CompressionMethodShrunk = 1,
    CompressionMethodReduced1 = 2,
    CompressionMethodReduced2 = 3,
    CompressionMethodReduced3 = 4,
    CompressionMethodReduced4 = 5,
    CompressionMethodImploded = 6,
    CompressionMethodReservedTokenizing = 7, // reserved for tokenizing
    CompressionMethodDeflated = 8,
    CompressionMethodDeflated64 = 9,
    CompressionMethodPKImploding = 10,

    CompressionMethodBZip2 = 12,

    CompressionMethodLZMA = 14,

    CompressionMethodTerse = 18,
    CompressionMethodLz77 = 19,

    CompressionMethodJpeg = 96,
    CompressionMethodWavPack = 97,
    CompressionMethodPPMd = 98,
    CompressionMethodWzAES = 99
};

// Hard limits for reading a single entry. The central directory is authoritative.
// 64 MiB for the uncompressed data; the compressed data gets the same baseline plus
// the bounded worst-case raw-deflate overhead (the same bound used by addEntry).
static constexpr uint32_t MAX_ENTRY_UNCOMPRESSED_SIZE = 64u * 1024u * 1024u;
static constexpr uint32_t MAX_ENTRY_COMPRESSED_SIZE = MAX_ENTRY_UNCOMPRESSED_SIZE
                                                      + (MAX_ENTRY_UNCOMPRESSED_SIZE >> 12)
                                                      + (MAX_ENTRY_UNCOMPRESSED_SIZE >> 14) + 11u;

static constexpr uint32_t ENCRYPTION_FLAGS = uint32_t(Encrypted) | uint32_t(StrongEncrypted)
                                             | uint32_t(CentralDirectoryEncrypted);

static constexpr uint64_t MAX_TOTAL_UNCOMPRESSED_SIZE = 256ULL * 1024ULL * 1024ULL;

struct LocalFileHeader
{
    uint8_t signature[4]; //  0x04034b50
    uint8_t version_needed[2];
    uint8_t general_purpose_bits[2];
    uint8_t compression_method[2];
    uint8_t last_mod_file[4];
    uint8_t crc_32[4];
    uint8_t compressed_size[4];
    uint8_t uncompressed_size[4];
    uint8_t file_name_length[2];
    uint8_t extra_field_length[2];
};

bool ZipContainer::startsWithEntry(const uint8_t* data, size_t size, std::span<const uint8_t> name)
{
    if (size < sizeof(LocalFileHeader) + name.size()) {
        return false;
    }

    return readUInt(data + offsetof(LocalFileHeader, signature)) == 0x04034b50
           && readUShort(data + offsetof(LocalFileHeader, file_name_length)) == name.size()
           && std::equal(name.begin(), name.end(), data + sizeof(LocalFileHeader));
}

struct DataDescriptor
{
    uint8_t crc_32[4];
    uint8_t compressed_size[4];
    uint8_t uncompressed_size[4];
};

struct CentralFileHeader
{
    uint8_t signature[4]; // 0x02014b50
    uint8_t version_made[2];
    uint8_t version_needed[2];
    uint8_t general_purpose_bits[2];
    uint8_t compression_method[2];
    uint8_t last_mod_file[4];
    uint8_t crc_32[4];
    uint8_t compressed_size[4];
    uint8_t uncompressed_size[4];
    uint8_t file_name_length[2];
    uint8_t extra_field_length[2];
    uint8_t file_comment_length[2];
    uint8_t disk_start[2];
    uint8_t internal_file_attributes[2];
    uint8_t external_file_attributes[4];
    uint8_t offset_local_header[4];
    LocalFileHeader toLocalHeader() const;
};

struct EndOfDirectory
{
    uint8_t signature[4]; // 0x06054b50
    uint8_t this_disk[2];
    uint8_t start_of_directory_disk[2];
    uint8_t num_dir_entries_this_disk[2];
    uint8_t num_dir_entries[2];
    uint8_t directory_size[4];
    uint8_t dir_start_offset[4];
    uint8_t comment_length[2];
};

struct FileHeader
{
    CentralFileHeader h;
    ByteArray file_name;
    ByteArray extra_field;
    ByteArray file_comment;
};

LocalFileHeader CentralFileHeader::toLocalHeader() const
{
    LocalFileHeader h;
    writeUInt(h.signature, 0x04034b50);
    copyUShort(h.version_needed, version_needed);
    copyUShort(h.general_purpose_bits, general_purpose_bits);
    copyUShort(h.compression_method, compression_method);
    copyUInt(h.last_mod_file, last_mod_file);
    copyUInt(h.crc_32, crc_32);
    copyUInt(h.compressed_size, compressed_size);
    copyUInt(h.uncompressed_size, uncompressed_size);
    copyUShort(h.file_name_length, file_name_length);
    copyUShort(h.extra_field_length, extra_field_length);
    return h;
}

struct ZipContainer::Impl {
    IODevice* device = nullptr;

    bool dirtyFileTree = true;
    std::vector<FileHeader> fileHeaders;
    ByteArray comment;
    uint start_of_directory = 0;
    ZipContainer::Status status = ZipContainer::NoError;
    uint64_t totalDecompressedBytes = 0;

    ZipContainer::CompressionPolicy compressionPolicy = ZipContainer::AlwaysCompress;

    enum EntryType {
        Directory, File, Symlink
    };

    void addEntry(EntryType type, const std::string& fileName, const ByteArray& contents);
    bool writeToDevice(const uint8_t* data, size_t len);
    bool writeToDevice(const ByteArray& data);

    Impl(IODevice* d)
        : device(d) {}

    void scanFiles();
    ZipContainer::FileInfo fillFileInfo(size_t index) const;

    std::string fixFilePath(const ByteArray& path) const;
};

void ZipContainer::Impl::scanFiles()
{
    if (!dirtyFileTree) {
        return;
    }

    if (status != ZipContainer::NoError) {
        return;
    }

    if (!(device->isOpen() || device->open(IODevice::ReadOnly))) {
        status = ZipContainer::FileOpenError;
        fileHeaders.clear();
        return;
    }

    if ((device->openMode() & IODevice::ReadOnly) == 0) { // only read the index from readable files.
        status = ZipContainer::FileReadError;
        fileHeaders.clear();
        return;
    }

    dirtyFileTree = false;

    const auto fail = [this](const char* message) {
        LOGW() << "Zip: " << message;
        fileHeaders.clear();
        comment.clear();
        status = ZipContainer::FileReadError;
    };

    const uint64_t fileSize = device->size();
    if (fileSize < sizeof(EndOfDirectory)) {
        fail("File is smaller than minimum ZIP size.");
        return;
    }

    if (!device->seek(0)) {
        fail("Failed to seek to start of ZIP file.");
        return;
    }

    uint8_t tmp[4];
    if (device->read(tmp, sizeof(tmp)) != sizeof(tmp)) {
        fail("Failed to read ZIP signature.");
        return;
    }

    const uint32_t firstSig = readUInt(tmp);
    if (firstSig != 0x04034b50 && firstSig != 0x06054b50) {
        fail("Invalid ZIP signature.");
        return;
    }

    EndOfDirectory eod;
    std::memset(&eod, 0, sizeof(EndOfDirectory));
    bool foundEod = false;
    uint64_t eodOffset = 0;
    const uint64_t maxComment = std::min<uint64_t>(65535, fileSize - sizeof(EndOfDirectory));
    for (uint64_t i = 0; i <= maxComment; ++i) {
        const uint64_t candidatePos = fileSize - sizeof(EndOfDirectory) - i;
        if (!device->seek(static_cast<size_t>(candidatePos))) {
            fail("Failed to seek while searching for EndOfDirectory.");
            return;
        }
        if (device->read(reinterpret_cast<uint8_t*>(&eod), sizeof(EndOfDirectory)) != sizeof(EndOfDirectory)) {
            fail("Failed to read while searching for EndOfDirectory.");
            return;
        }
        if (readUInt(eod.signature) != 0x06054b50) {
            continue;
        }
        // A signature inside the archive comment is not the record. The declared
        // comment length must exactly span the bytes after this candidate;
        // otherwise keep scanning backwards for the real record.
        if (readUShort(eod.comment_length) != i) {
            continue;
        }
        foundEod = true;
        eodOffset = candidatePos;
        if (i > 0) {
            comment = device->read(static_cast<size_t>(i));
            if (comment.size() != i) {
                fail("Failed to read archive comment.");
                return;
            }
        }
        break;
    }

    if (!foundEod) {
        fail("EndOfDirectory not found.");
        return;
    }

    const uint16_t thisDisk = readUShort(eod.this_disk);
    const uint16_t directoryDisk = readUShort(eod.start_of_directory_disk);
    const uint16_t entriesThisDisk = readUShort(eod.num_dir_entries_this_disk);
    const uint16_t numDirEntries = readUShort(eod.num_dir_entries);
    if (thisDisk != 0 || directoryDisk != 0 || entriesThisDisk != numDirEntries) {
        fail("Multi-disk ZIP archives are unsupported.");
        return;
    }

    const uint64_t dirStartOffset = readUInt(eod.dir_start_offset);
    const uint64_t directorySize = readUInt(eod.directory_size);

    if (dirStartOffset + directorySize > eodOffset) {
        fail("Central directory extends beyond EndOfDirectory offset.");
        return;
    }

    if (directorySize < uint64_t(numDirEntries) * sizeof(CentralFileHeader)) {
        fail("Central directory size too small for declared number of entries.");
        return;
    }

    start_of_directory = static_cast<uint>(dirStartOffset);
    if (!device->seek(static_cast<size_t>(dirStartOffset))) {
        fail("Failed to seek to central directory.");
        return;
    }

    fileHeaders.clear();
    fileHeaders.reserve(numDirEntries);
    uint64_t totalUncompressedSize = 0;

    for (uint16_t i = 0; i < numDirEntries; ++i) {
        FileHeader header;
        if (device->read(reinterpret_cast<uint8_t*>(&header.h), sizeof(CentralFileHeader)) != sizeof(CentralFileHeader)) {
            fail("Failed to read complete central directory header.");
            return;
        }

        if (readUInt(header.h.signature) != 0x02014b50) {
            fail("Invalid central directory header signature.");
            return;
        }

        if (readUShort(header.h.disk_start) != 0) {
            fail("Central directory entry belongs to another disk.");
            return;
        }

        const size_t nameLen = readUShort(header.h.file_name_length);
        header.file_name = device->read(nameLen);
        if (header.file_name.size() != nameLen) {
            fail("Failed to read filename from central directory.");
            return;
        }

        const size_t extraLen = readUShort(header.h.extra_field_length);
        header.extra_field = device->read(extraLen);
        if (header.extra_field.size() != extraLen) {
            fail("Failed to read extra field from central directory.");
            return;
        }

        const size_t commentLen = readUShort(header.h.file_comment_length);
        header.file_comment = device->read(commentLen);
        if (header.file_comment.size() != commentLen) {
            fail("Failed to read file comment from central directory.");
            return;
        }

        if (device->pos() > dirStartOffset + directorySize) {
            fail("Central directory entry extends past declared directory size.");
            return;
        }

        const uint32_t entryUncompressedSize = readUInt(header.h.uncompressed_size);
        if (entryUncompressedSize > MAX_ENTRY_UNCOMPRESSED_SIZE) {
            fail("Central directory entry uncompressed size exceeds limit.");
            return;
        }

        totalUncompressedSize += entryUncompressedSize;
        if (totalUncompressedSize > MAX_TOTAL_UNCOMPRESSED_SIZE) {
            fail("Aggregate uncompressed size across entries exceeds limit.");
            return;
        }

        fileHeaders.push_back(std::move(header));
    }
}

ZipContainer::FileInfo ZipContainer::Impl::fillFileInfo(size_t index) const
{
    ZipContainer::FileInfo fileInfo;
    FileHeader header = fileHeaders.at(index);
    uint32_t mode = readUInt(header.h.external_file_attributes);
    const HostOS hostOS = HostOS(readUShort(header.h.version_made) >> 8);
    switch (hostOS) {
    case HostUnix:
        mode = (mode >> 16) & 0xffff;
        switch (mode & UnixFileAttributes::TypeMask) {
        case UnixFileAttributes::SymLink:
            fileInfo.isSymLink = true;
            break;
        case UnixFileAttributes::Dir:
            fileInfo.isDir = true;
            break;
        case UnixFileAttributes::File:
        default: // ### just for the case; should we warn?
            fileInfo.isFile = true;
            break;
        }
        break;
    case HostFAT:
    case HostNTFS:
    case HostHPFS:
    case HostVFAT:
        switch (mode & WindowsFileAttributes::TypeMask) {
        case WindowsFileAttributes::Dir:
            fileInfo.isDir = true;
            break;
        case WindowsFileAttributes::File:
        default:
            fileInfo.isFile = true;
            break;
        }

        break;
    default:
        LOGW("Zip: Zip entry format at %zu is not supported.", index);
        return fileInfo; // we don't support anything else
    }

    // ushort general_purpose_bits = readUShort(header.h.general_purpose_bits);
    // if bit 11 is set, the filename and comment fields must be encoded using UTF-8
    // const bool inUtf8 = (general_purpose_bits & Utf8Names) != 0;
    fileInfo.filePath = fixFilePath(header.file_name);
    fileInfo.crc = readUInt(header.h.crc_32);
    fileInfo.size = readUInt(header.h.uncompressed_size);
    fileInfo.lastModified = readMSDosDate(header.h.last_mod_file);

    return fileInfo;
}

void ZipContainer::Impl::addEntry(EntryType type, const std::string& fileName, const ByteArray& contents)
{
    if (!(device->isOpen() || device->open(IODevice::WriteOnly))) {
        status = ZipContainer::FileOpenError;
        return;
    }
    device->seek(start_of_directory);

    // don't compress small files
    ZipContainer::CompressionPolicy compression = compressionPolicy;
    if (compressionPolicy == ZipContainer::AutoCompress) {
        if (contents.size() < 64) {
            compression = ZipContainer::NeverCompress;
        } else {
            compression = ZipContainer::AlwaysCompress;
        }
    }

    FileHeader header;
    std::memset(&header.h, 0, sizeof(CentralFileHeader));
    writeUInt(header.h.signature, 0x02014b50);

    writeUShort(header.h.version_needed, ZIP_VERSION);
    writeUInt(header.h.uncompressed_size, (uint)contents.size());

    std::time_t t = std::time(0);   // get time now
    std::tm now;
#ifdef WIN32
    localtime_s(&now, &t);
#else
    localtime_r(&t, &now);
#endif
    writeMSDosDate(header.h.last_mod_file, now);
    ByteArray data = contents;
    if (compression == ZipContainer::AlwaysCompress) {
        writeUShort(header.h.compression_method, CompressionMethodDeflated);

        ulong len = (ulong)contents.size();
        // shamelessly copied form zlib
        len += (len >> 12) + (len >> 14) + 11;
        int res;
        do {
            data.resize(len);
            res = deflate((uint8_t*)data.data(), &len, (const uint8_t*)contents.constData(), (ulong)contents.size());

            switch (res) {
            case Z_OK:
                data.resize(len);
                break;
            case Z_MEM_ERROR:
                LOGW("Zip: Z_MEM_ERROR: Not enough memory to compress file, skipping");
                data.resize(0);
                break;
            case Z_BUF_ERROR:
                len *= 2;
                break;
            }
        } while (res == Z_BUF_ERROR);
    }
// TODO add a check if data.size() > contents.size().  Then try to store the original and revert the compression method to be uncompressed
    writeUInt(header.h.compressed_size, (uint)data.size());
    uint crc_32 = ::crc32(0, 0, 0);
    crc_32 = ::crc32(crc_32, (const uint8_t*)contents.constData(), (uint)contents.size());
    writeUInt(header.h.crc_32, crc_32);

    // if bit 11 is set, the filename and comment fields must be encoded using UTF-8
    ushort general_purpose_bits = Utf8Names; // always use utf-8
    writeUShort(header.h.general_purpose_bits, general_purpose_bits);

    //const bool inUtf8 = (general_purpose_bits & Utf8Names) != 0;
    header.file_name = ByteArray(fileName.c_str(), fileName.size());
    if (header.file_name.size() > 0xffff) {
        LOGW("Zip: Filename is too long, chopping it to 65535 bytes");
        header.file_name = header.file_name.left(0xffff); // ### don't break the utf-8 sequence, if any
    }
    if (header.file_comment.size() + header.file_name.size() > 0xffff) {
        LOGW("Zip: File comment is too long, chopping it to 65535 bytes");
        header.file_comment.truncate(0xffff - header.file_name.size()); // ### don't break the utf-8 sequence, if any
    }
    writeUShort(header.h.file_name_length, (ushort)header.file_name.size());
    //h.extra_field_length[2];

    writeUShort(header.h.version_made, HostUnix << 8);
    //uint8_t internal_file_attributes[2];
    //uint8_t external_file_attributes[4];
    uint32_t mode = UnixFileAttributes::ReadUser
                    | UnixFileAttributes::WriteUser
                    | UnixFileAttributes::ExeUser
                    | UnixFileAttributes::ReadGroup
                    | UnixFileAttributes::ReadOther;
    switch (type) {
    case Symlink:
        mode |= UnixFileAttributes::SymLink;
        break;
    case Directory:
        mode |= UnixFileAttributes::Dir;
        break;
    case File:
        mode |= UnixFileAttributes::File;
        break;
    default:
        UNREACHABLE;
        break;
    }
    writeUInt(header.h.external_file_attributes, mode << 16);
    writeUInt(header.h.offset_local_header, start_of_directory);

    fileHeaders.push_back(header);

    bool ok = true;

    LocalFileHeader h = header.h.toLocalHeader();
    ok &= writeToDevice((const uint8_t*)&h, sizeof(LocalFileHeader));
    ok &= writeToDevice(header.file_name);
    ok &= writeToDevice(data);

    start_of_directory = (uint)device->pos();
    dirtyFileTree = true;

    if (!ok) {
        status = ZipContainer::FileWriteError;
    }
}

bool ZipContainer::Impl::writeToDevice(const uint8_t* data, size_t len)
{
    return device->write(data, len) == len;
}

bool ZipContainer::Impl::writeToDevice(const ByteArray& data)
{
    return device->write(data) == data.size();
}

std::string ZipContainer::Impl::fixFilePath(const ByteArray& path) const
{
    // fix the file path, if broken (convert separators, eat leading and trailing ones)
    std::string fixed = Dir::fromNativeSeparators(path.constChar()).toStdString();
    {
        bool frontOk = false;
        while (!fixed.empty() && !frontOk) {
            if (fixed.front() == '/') {
                fixed = fixed.substr(1);
            } else if (fixed.rfind("./", 0) == 0) {
                fixed = fixed.substr(2);
            } else if (fixed.rfind("../", 0) == 0) {
                fixed = fixed.substr(3);
            } else {
                frontOk = true;
            }
        }
    }

    while (!fixed.empty() && fixed.back() == '/') {
        fixed = fixed.substr(0, fixed.size() - 1);
    }

    return fixed;
}

ZipContainer::ZipContainer(IODevice* device)
    : p(new Impl(device))
{
    assert(device);
}

ZipContainer::~ZipContainer()
{
    close();
    delete p;
}

std::vector<ZipContainer::FileInfo> ZipContainer::fileInfoList() const
{
    p->scanFiles();
    std::vector<FileInfo> files;
    const size_t numFileHeaders = p->fileHeaders.size();
    files.reserve(numFileHeaders);
    for (size_t i = 0; i < numFileHeaders; ++i) {
        files.push_back(p->fillFileInfo(i));
    }
    return files;
}

int ZipContainer::count() const
{
    p->scanFiles();
    return (int)p->fileHeaders.size();
}

bool ZipContainer::fileExists(const std::string& fileName) const
{
    p->scanFiles();

    for (size_t i = 0; i < p->fileHeaders.size(); ++i) {
        std::string currentFileName = p->fixFilePath(p->fileHeaders.at(i).file_name.constChar());
        if (currentFileName == fileName) {
            return true;
        }
    }

    return false;
}

ByteArray ZipContainer::fileData(const std::string& fileName) const
{
    if (p->status != ZipContainer::NoError) {
        return ByteArray();
    }

    p->scanFiles();

    if (p->status != ZipContainer::NoError) {
        return ByteArray();
    }

    size_t i = 0;
    for (i = 0; i < p->fileHeaders.size(); ++i) {
        std::string currentFileName = p->fixFilePath(p->fileHeaders.at(i).file_name.constChar());
        if (currentFileName == fileName) {
            break;
        }
    }

    if (i == p->fileHeaders.size()) {
        return ByteArray();
    }

    const auto fail = [this](const char* message) -> ByteArray {
        LOGW() << "Zip: " << message;
        p->status = ZipContainer::FileReadError;
        return ByteArray();
    };

    const FileHeader& header = p->fileHeaders.at(i);

    const uint32_t central_version = readUShort(header.h.version_needed);
    if (central_version > ZIP_VERSION) {
        LOGW() << "Zip: .ZIP specification version " << central_version
               << " implementation is needed to extract the data.";
        p->status = ZipContainer::FileReadError;
        return ByteArray();
    }

    const uint32_t central_flags = readUShort(header.h.general_purpose_bits);
    if ((central_flags & ENCRYPTION_FLAGS) != 0) {
        return fail("Unsupported encryption method is needed to extract the data.");
    }

    const uint32_t central_method = readUShort(header.h.compression_method);
    if (central_method != CompressionMethodStored && central_method != CompressionMethodDeflated) {
        LOGW() << "Zip: Unsupported compression method " << central_method
               << " is needed to extract the data.";
        p->status = ZipContainer::FileReadError;
        return ByteArray();
    }

    const uint32_t central_crc = readUInt(header.h.crc_32);
    const uint32_t compressed_size = readUInt(header.h.compressed_size);
    const uint32_t uncompressed_size = readUInt(header.h.uncompressed_size);
    const uint32_t local_offset = readUInt(header.h.offset_local_header);

    if (uncompressed_size > MAX_ENTRY_UNCOMPRESSED_SIZE) {
        return fail("Uncompressed entry exceeds the supported size limit.");
    }

    if (p->totalDecompressedBytes + uncompressed_size > MAX_TOTAL_UNCOMPRESSED_SIZE) {
        return fail("Aggregate decompressed size exceeds limit.");
    }

    if (compressed_size > MAX_ENTRY_COMPRESSED_SIZE) {
        return fail("Compressed entry exceeds the supported size limit.");
    }

    if (central_method == CompressionMethodStored && compressed_size != uncompressed_size) {
        return fail("Stored entry has different compressed and uncompressed sizes.");
    }

    IODevice* device = p->device;
    const uint64_t file_size = device->size();
    if (uint64_t(local_offset) > file_size
        || file_size - uint64_t(local_offset) < sizeof(LocalFileHeader)) {
        return fail("Local file header offset is outside the file.");
    }

    if (!device->seek(static_cast<size_t>(local_offset))) {
        return fail("Failed to seek to the local file header.");
    }

    LocalFileHeader lh;
    std::memset(&lh, 0, sizeof(lh));
    if (device->read(reinterpret_cast<uint8_t*>(&lh), sizeof(lh)) != sizeof(lh)) {
        return fail("Failed to read the complete local file header.");
    }

    if (readUInt(lh.signature) != 0x04034b50) {
        return fail("Invalid local file header signature.");
    }

    const uint32_t local_version = readUShort(lh.version_needed);
    if (local_version > ZIP_VERSION) {
        LOGW() << "Zip: .ZIP specification version " << local_version
               << " implementation is needed to extract the data.";
        p->status = ZipContainer::FileReadError;
        return ByteArray();
    }

    const uint32_t local_flags = readUShort(lh.general_purpose_bits);
    if ((local_flags & ENCRYPTION_FLAGS) != 0) {
        return fail("Unsupported encryption method is needed to extract the data.");
    }

    const uint32_t flag_mask = ENCRYPTION_FLAGS | uint32_t(HasDataDescriptor);
    if ((central_flags & flag_mask) != (local_flags & flag_mask)) {
        return fail("Local and central general purpose flags are inconsistent.");
    }

    const uint32_t local_method = readUShort(lh.compression_method);
    if (local_method != central_method) {
        return fail("Local and central compression methods are inconsistent.");
    }

    if (local_method != CompressionMethodStored && local_method != CompressionMethodDeflated) {
        LOGW() << "Zip: Unsupported compression method " << local_method
               << " is needed to extract the data.";
        p->status = ZipContainer::FileReadError;
        return ByteArray();
    }

    // With a data descriptor, the central directory is authoritative and the local
    // CRC/sizes may be zero. Without one, the local header must match the central one.
    if ((central_flags & HasDataDescriptor) == 0) {
        if (readUInt(lh.crc_32) != central_crc
            || readUInt(lh.compressed_size) != compressed_size
            || readUInt(lh.uncompressed_size) != uncompressed_size) {
            return fail("Local file header CRC or sizes do not match the central directory.");
        }
    }

    const uint32_t local_name_length = readUShort(lh.file_name_length);
    const uint32_t local_extra_length = readUShort(lh.extra_field_length);
    const uint64_t data_offset = uint64_t(local_offset) + sizeof(LocalFileHeader)
                                 + local_name_length + local_extra_length;
    if (data_offset > file_size || compressed_size > file_size - data_offset) {
        return fail("Compressed data extends past the end of the file.");
    }

    if (!device->seek(static_cast<size_t>(data_offset))) {
        return fail("Failed to seek to the compressed data.");
    }

    ByteArray compressed = device->read(static_cast<size_t>(compressed_size));
    if (compressed.size() != compressed_size) {
        return fail("Failed to read the complete compressed data.");
    }

    ByteArray result;
    if (central_method == CompressionMethodStored) {
        result = compressed;
    } else {
        // Allocate exactly the declared output size; an empty entry still needs a
        // valid one-byte destination for zlib. Inflate once with no retry growth.
        const size_t output_capacity = uncompressed_size > 0 ? uncompressed_size : 1;
        result.resize(output_capacity);
        ulong output_len = static_cast<ulong>(output_capacity);
        const int res = inflate(result.data(), &output_len, compressed.constData(),
                                static_cast<ulong>(compressed_size));
        if (res != Z_OK) {
            LOGW() << "Zip: inflate failed with error " << res << ".";
            p->status = ZipContainer::FileReadError;
            return ByteArray();
        }

        if (output_len != uncompressed_size) {
            return fail("Inflated data size does not match the central directory.");
        }

        result.resize(static_cast<size_t>(output_len));
    }

    uint32_t actual_crc = static_cast<uint32_t>(::crc32(0, Z_NULL, 0));
    if (!result.empty()) {
        actual_crc = static_cast<uint32_t>(::crc32(actual_crc, result.constData(),
                                                   static_cast<uInt>(result.size())));
    }

    if (actual_crc != central_crc) {
        return fail("CRC does not match the central directory.");
    }

    p->totalDecompressedBytes += result.size();

    return result;
}

ZipContainer::Status ZipContainer::status() const
{
    return p->status;
}

void ZipContainer::setCompressionPolicy(CompressionPolicy policy)
{
    p->compressionPolicy = policy;
}

ZipContainer::CompressionPolicy ZipContainer::compressionPolicy() const
{
    return p->compressionPolicy;
}

void ZipContainer::addFile(const std::string& fileName, const ByteArray& data)
{
    p->addEntry(Impl::File, Dir::fromNativeSeparators(fileName).toStdString(), data);
}

void ZipContainer::addDirectory(const std::string& dirName)
{
    std::string name(Dir::fromNativeSeparators(dirName).toStdString());
    // separator is mandatory
    if (name.back() != '/') {
        name.push_back('/');
    }
    p->addEntry(Impl::Directory, name, ByteArray());
}

void ZipContainer::close()
{
    if (!(p->device->openMode() & IODevice::WriteOnly)) {
        p->device->close();
        return;
    }

    bool ok = true;

    //qDebug("Zip::close writing directory, %d entries", p->fileHeaders.size());
    p->device->seek(p->start_of_directory);
    // write new directory
    for (size_t i = 0; i < p->fileHeaders.size(); ++i) {
        const FileHeader& header = p->fileHeaders.at(i);
        ok &= p->writeToDevice((const uint8_t*)&header.h, sizeof(CentralFileHeader));
        ok &= p->writeToDevice(header.file_name);
        ok &= p->writeToDevice(header.extra_field);
        ok &= p->writeToDevice(header.file_comment);
    }
    int dir_size = (int)p->device->pos() - (int)p->start_of_directory;
    // write end of directory
    EndOfDirectory eod;
    memset(&eod, 0, sizeof(EndOfDirectory));
    writeUInt(eod.signature, 0x06054b50);
    //uint8_t this_disk[2];
    //uint8_t start_of_directory_disk[2];
    writeUShort(eod.num_dir_entries_this_disk, (ushort)p->fileHeaders.size());
    writeUShort(eod.num_dir_entries, (ushort)p->fileHeaders.size());
    writeUInt(eod.directory_size, dir_size);
    writeUInt(eod.dir_start_offset, p->start_of_directory);
    writeUShort(eod.comment_length, (ushort)p->comment.size());

    ok &= p->writeToDevice((const uint8_t*)&eod, sizeof(EndOfDirectory));
    ok &= p->writeToDevice(p->comment);
    p->device->close();

    if (!ok) {
        p->status = ZipContainer::FileWriteError;
    }
}
}
