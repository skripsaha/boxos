/*
 * file_metadata.c — lossless userspace view of a TagFS file record.
 *
 * STORAGE_OBJ_GET_INFO returns a self-describing record whose size depends on
 * the filename and every tag spelling.  file_info_t intentionally has fixed
 * display-sized fields; this adapter is for code that must carry the record
 * forward without changing a byte of its meaning.
 */

#include "box/file.h"
#include "box/core/manifest.h"
#include "box/error.h"
#include "box/memory.h"
#include "box/string.h"
#include "box/timeouts.h"
#include "boxos_decks.h"

#define FILE_METADATA_FIRST_CAPACITY 512u

static int ReadMetadataBlob(file_id_t FileId, uint8_t **OutBlob,
                            uint32_t *OutSize)
{
    uint32_t Capacity = FILE_METADATA_FIRST_CAPACITY;

    if (!OutBlob || !OutSize)
        return -ERR_INVALID_ARGUMENT;
    *OutBlob = NULL;
    *OutSize = 0;

    for (;;) {
        uint8_t *Blob = (uint8_t *)malloc(Capacity);
        uint32_t Actual = 0;
        int Rc;

        if (!Blob)
            return -ERR_NO_MEMORY;

        Rc = MfCall1(DECK_STORAGE, STORAGE_OBJ_GET_INFO,
                     &FileId, sizeof(FileId),
                     NULL, 0,
                     Blob, Capacity, &Actual,
                     BOX_ANSWER_GUARANTEED, NULL);
        if (Rc == 0) {
            if (Actual == 0) {
                free(Blob);
                return -ERR_INTERNAL;
            }
            *OutBlob = Blob;
            *OutSize = Actual;
            return 0;
        }

        free(Blob);
        if (Rc != ERR_BUFFER_TOO_SMALL)
            return box_fail(Rc);

        if (Capacity > UINT32_MAX / 2u)
            return -ERR_BUFFER_TOO_SMALL;
        Capacity *= 2u;
    }
}

static int ReadU16(const uint8_t *Blob, uint32_t Size, uint32_t *Position,
                   uint16_t *Out)
{
    if (!Blob || !Position || !Out || *Position > Size || Size - *Position < 2u)
        return -ERR_INTERNAL;
    memcpy(Out, Blob + *Position, sizeof(*Out));
    *Position += sizeof(*Out);
    return 0;
}

static int ReadBytes(const uint8_t *Blob, uint32_t Size, uint32_t *Position,
                     uint32_t Count, const uint8_t **Out)
{
    if (!Blob || !Position || !Out || *Position > Size || Count > Size - *Position)
        return -ERR_INTERNAL;
    *Out = Blob + *Position;
    *Position += Count;
    return 0;
}

static int MetadataTagBytes(const uint8_t *Blob, uint32_t Size,
                            uint32_t Position, uint16_t TagCount,
                            size_t *OutBytes)
{
    size_t Bytes = 0;
    uint16_t Index;

    if (!Blob || !OutBytes)
        return -ERR_INVALID_ARGUMENT;

    for (Index = 0; Index < TagCount; Index++) {
        uint16_t KeyLength;
        uint16_t ValueLength;
        const uint8_t *Ignored;
        uint32_t RecordBytes;

        if (ReadU16(Blob, Size, &Position, &KeyLength) != 0 ||
            ReadU16(Blob, Size, &Position, &ValueLength) != 0)
            return -ERR_INTERNAL;
        if (ReadBytes(Blob, Size, &Position, 1u, &Ignored) != 0)
            return -ERR_INTERNAL;
        if (KeyLength == 0)
            return -ERR_INTERNAL;

        RecordBytes = (uint32_t)KeyLength + (uint32_t)ValueLength;
        if (ReadBytes(Blob, Size, &Position, RecordBytes, &Ignored) != 0)
            return -ERR_INTERNAL;

        if (Bytes > SIZE_MAX - (size_t)KeyLength - (size_t)ValueLength)
            return -ERR_NO_MEMORY;
        Bytes += (size_t)KeyLength + (size_t)ValueLength;
        if (ValueLength != 0) {
            if (Bytes == SIZE_MAX)
                return -ERR_NO_MEMORY;
            Bytes++;
        }
        if (Index != 0) {
            if (Bytes == SIZE_MAX)
                return -ERR_NO_MEMORY;
            Bytes++;
        }
    }

    *OutBytes = Bytes;
    return 0;
}

int FileMetadataRead(file_id_t FileId, FileMetadata *Out)
{
    uint8_t *Blob = NULL;
    uint32_t BlobSize = 0;
    uint32_t Position = 0;
    uint16_t TagCount;
    uint16_t FilenameLength;
    const uint8_t *FilenameBytes;
    size_t TagBytes = 0;
    char *Filename = NULL;
    char *Tags = NULL;
    uint16_t Index;
    int Rc;

    if (!Out)
        return -ERR_INVALID_ARGUMENT;
    memset(Out, 0, sizeof(*Out));

    Rc = ReadMetadataBlob(FileId, &Blob, &BlobSize);
    if (Rc != 0)
        return Rc;

    if (BlobSize < 24u) {
        free(Blob);
        return -ERR_INTERNAL;
    }

    memcpy(&Out->FileId, Blob + Position, sizeof(Out->FileId));
    Position += sizeof(Out->FileId);
    memcpy(&Out->Flags, Blob + Position, sizeof(Out->Flags));
    Position += sizeof(Out->Flags);
    memcpy(&Out->Size, Blob + Position, sizeof(Out->Size));
    Position += sizeof(Out->Size);
    if (ReadU16(Blob, BlobSize, &Position, &TagCount) != 0 ||
        ReadU16(Blob, BlobSize, &Position, &FilenameLength) != 0) {
        free(Blob);
        return -ERR_INTERNAL;
    }

    if (ReadBytes(Blob, BlobSize, &Position, FilenameLength, &FilenameBytes) != 0) {
        free(Blob);
        return -ERR_INTERNAL;
    }
    Rc = MetadataTagBytes(Blob, BlobSize, Position, TagCount, &TagBytes);
    if (Rc != 0) {
        free(Blob);
        return Rc;
    }

    Filename = (char *)malloc((size_t)FilenameLength + 1u);
    if (!Filename) {
        free(Blob);
        return -ERR_NO_MEMORY;
    }
    memcpy(Filename, FilenameBytes, FilenameLength);
    Filename[FilenameLength] = '\0';

    if (TagBytes != 0) {
        Tags = (char *)malloc(TagBytes + 1u);
        if (!Tags) {
            free(Filename);
            free(Blob);
            return -ERR_NO_MEMORY;
        }
    }

    Position = 24u + FilenameLength;
    size_t TagPosition = 0;
    for (Index = 0; Index < TagCount; Index++) {
        uint16_t KeyLength;
        uint16_t ValueLength;
        const uint8_t *Key;
        const uint8_t *Value;

        if (ReadU16(Blob, BlobSize, &Position, &KeyLength) != 0 ||
            ReadU16(Blob, BlobSize, &Position, &ValueLength) != 0 ||
            ReadBytes(Blob, BlobSize, &Position, 1u, &Value) != 0 ||
            ReadBytes(Blob, BlobSize, &Position, KeyLength, &Key) != 0 ||
            ReadBytes(Blob, BlobSize, &Position, ValueLength, &Value) != 0) {
            free(Tags);
            free(Filename);
            free(Blob);
            return -ERR_INTERNAL;
        }

        if (KeyLength == 0) {
            free(Tags);
            free(Filename);
            free(Blob);
            return -ERR_INTERNAL;
        }

        if (Index != 0)
            Tags[TagPosition++] = ',';
        memcpy(Tags + TagPosition, Key, KeyLength);
        TagPosition += KeyLength;
        if (ValueLength != 0) {
            Tags[TagPosition++] = ':';
            memcpy(Tags + TagPosition, Value, ValueLength);
            TagPosition += ValueLength;
        }
    }
    if (Position != BlobSize || TagPosition != TagBytes) {
        free(Tags);
        free(Filename);
        free(Blob);
        return -ERR_INTERNAL;
    }
    if (Tags)
        Tags[TagPosition] = '\0';

    Out->TagCount = TagCount;
    Out->Filename = Filename;
    Out->Tags = Tags;
    free(Blob);
    return 0;
}

void FileMetadataRelease(FileMetadata *Metadata)
{
    if (!Metadata)
        return;
    free(Metadata->Filename);
    free(Metadata->Tags);
    memset(Metadata, 0, sizeof(*Metadata));
}
