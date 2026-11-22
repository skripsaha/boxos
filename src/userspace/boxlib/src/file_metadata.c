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

static int ReadMetadataBlob(TagsOf Whose, file_id_t FileId, uint8_t **OutBlob,
                            uint32_t *OutSize)
{
    uint32_t Capacity = FILE_METADATA_FIRST_CAPACITY;
    uint8_t  Params[17];
    uint32_t ParamsSize = 9;

    if (!OutBlob || !OutSize)
        return -ERR_INVALID_ARGUMENT;
    *OutBlob = NULL;
    *OutSize = 0;
    memcpy(Params, &FileId, sizeof(FileId));
    Params[8] = Whose.whose;
    if (Whose.whose == STORAGE_TAGS_PROGRAM) {
        memcpy(Params + 9, &Whose.program, sizeof(Whose.program));
        ParamsSize = 17;
    }

    for (;;) {
        uint8_t *Blob = (uint8_t *)malloc(Capacity);
        uint32_t Actual = 0;
        int Rc;

        if (!Blob)
            return -ERR_NO_MEMORY;

        Rc = MfCall1(DECK_STORAGE, STORAGE_OBJ_GET_INFO,
                     Params, ParamsSize,
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

static int ReadU32(const uint8_t *Blob, uint32_t Size, uint32_t *Position,
                   uint32_t *Out)
{
    if (!Blob || !Position || !Out || *Position > Size || Size - *Position < 4u)
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
                            uint32_t Position, uint32_t TagCount,
                            size_t *OutBytes)
{
    size_t Bytes = 0;
    uint32_t Index;

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
        if (ReadBytes(Blob, Size, &Position, 1u + sizeof(file_id_t), &Ignored) != 0)   /* its type and owner */
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

int FileMetadataRead(TagsOf Whose, file_id_t FileId, FileMetadata *Out)
{
    uint8_t *Blob = NULL;
    uint32_t BlobSize = 0;
    uint32_t Position = 0;
    uint32_t TagCount;
    uint16_t FilenameLength;
    const uint8_t *FilenameBytes;
    size_t TagBytes = 0;
    char *Filename = NULL;
    char *Tags = NULL;
    file_id_t *Owners = NULL;
    uint32_t Index;
    int Rc;

    if (!Out)
        return -ERR_INVALID_ARGUMENT;
    memset(Out, 0, sizeof(*Out));

    Rc = ReadMetadataBlob(Whose, FileId, &Blob, &BlobSize);
    if (Rc != 0)
        return Rc;

    if (BlobSize < 30u) {
        free(Blob);
        return -ERR_INTERNAL;
    }

    memcpy(&Out->FileId, Blob + Position, sizeof(Out->FileId));
    Position += sizeof(Out->FileId);
    memcpy(&Out->Size, Blob + Position, sizeof(Out->Size));
    Position += sizeof(Out->Size);
    if (ReadU32(Blob, BlobSize, &Position, &TagCount) != 0 ||
        ReadU16(Blob, BlobSize, &Position, &FilenameLength) != 0) {
        free(Blob);
        return -ERR_INTERNAL;
    }

    if (ReadBytes(Blob, BlobSize, &Position, FilenameLength, &FilenameBytes) != 0) {
        free(Blob);
        return -ERR_INTERNAL;
    }
    const uint32_t TagsAt = Position;   /* the first tag's record, right after the name */
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
        Tags   = (char *)malloc(TagBytes + 1u);
        Owners = (file_id_t *)malloc(sizeof(file_id_t) * TagCount);
        if (!Tags || !Owners) {
            free(Tags);
            free(Owners);
            free(Filename);
            free(Blob);
            return -ERR_NO_MEMORY;
        }
    }

    Position = TagsAt;
    size_t TagPosition = 0;
    for (Index = 0; Index < TagCount; Index++) {
        uint16_t KeyLength;
        uint16_t ValueLength;
        const uint8_t *Key;
        const uint8_t *Value;
        const uint8_t *Owner;

        if (ReadU16(Blob, BlobSize, &Position, &KeyLength) != 0 ||
            ReadU16(Blob, BlobSize, &Position, &ValueLength) != 0 ||
            ReadBytes(Blob, BlobSize, &Position, 1u, &Value) != 0 ||
            ReadBytes(Blob, BlobSize, &Position, sizeof(file_id_t), &Owner) != 0 ||
            ReadBytes(Blob, BlobSize, &Position, KeyLength, &Key) != 0 ||
            ReadBytes(Blob, BlobSize, &Position, ValueLength, &Value) != 0 ||
            KeyLength == 0) {
            free(Tags);
            free(Owners);
            free(Filename);
            free(Blob);
            return -ERR_INTERNAL;
        }
        memcpy(&Owners[Index], Owner, sizeof(file_id_t));

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
    if (BlobSize - Position == sizeof(Out->KeptUntil) + sizeof(Out->MadeBy)) {
        memcpy(&Out->KeptUntil, Blob + Position, sizeof(Out->KeptUntil));
        Position += sizeof(Out->KeptUntil);
        memcpy(&Out->MadeBy, Blob + Position, sizeof(Out->MadeBy));
        Position += sizeof(Out->MadeBy);
    }
    if (Position != BlobSize || TagPosition != TagBytes) {
        free(Tags);
        free(Owners);
        free(Filename);
        free(Blob);
        return -ERR_INTERNAL;
    }
    if (Tags)
        Tags[TagPosition] = '\0';

    Out->TagCount = TagCount;
    Out->Filename = Filename;
    Out->Tags = Tags;
    Out->Owners = Owners;
    free(Blob);
    return 0;
}

void FileMetadataRelease(FileMetadata *Metadata)
{
    if (!Metadata)
        return;
    free(Metadata->Filename);
    free(Metadata->Tags);
    free(Metadata->Owners);
    memset(Metadata, 0, sizeof(*Metadata));
}

bool FileMetadataWears(const FileMetadata *Metadata, const char *Key)
{
    if (!Metadata || !Key) return false;
    size_t Length = strlen(Key);
    for (const char *At = Metadata->Tags; At && *At;) {
        const char *End  = strchr(At, ',');
        size_t      Word = End ? (size_t)(End - At) : strlen(At);
        if (Word >= Length && memcmp(At, Key, Length) == 0 && (Word == Length || At[Length] == ':'))
            return true;
        At = End ? End + 1 : NULL;
    }
    return false;
}
