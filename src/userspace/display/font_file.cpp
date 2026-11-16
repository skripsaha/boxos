/*
 * font_file.cpp — the person's font file found and read. See font_file.h.
 *
 * Every file wears the stem of its name, up to the last dot, as a tag, so
 * the files called JetBrainsMonoNL-Regular.ttf are among those that wear
 * JetBrainsMonoNL-Regular: one question asks for the stem and the person's
 * tags together, and of what answers, the files called so are the ones
 * meant. A file in the trash answers nothing: the person set it aside.
 */

#include "font_file.h"

#include <cstdlib>
#include <new>

#include "box/debug.h"
#include "box/file.h"

namespace {

// Whether the file is called Name.
bool Called(file_id_t File, const std::string &Name) noexcept
{
    FileMetadata Metadata;
    if (FileMetadataRead(File, &Metadata) != 0) return false;
    const bool Same = Metadata.Filename && Name == Metadata.Filename;
    FileMetadataRelease(&Metadata);
    return Same;
}

std::string Code(std::int64_t Refusal)
{
    return " (" + std::to_string(Refusal) + ")";
}

// The file's bytes, all of them.
std::expected<std::vector<std::uint8_t>, std::string> ReadWhole(file_id_t File, const std::string &Name)
{
    FileMetadata Metadata;
    const int    Described = FileMetadataRead(File, &Metadata);
    if (Described != 0) return std::unexpected(Name + " could not be described" + Code(Described));
    const std::uint64_t Size = Metadata.Size;
    FileMetadataRelease(&Metadata);

    std::vector<std::uint8_t> Bytes;
    try {
        Bytes.resize(Size);
    } catch (const std::bad_alloc &) {
        return std::unexpected("no room to read " + Name + ", " + std::to_string(Size) + " bytes");
    }
    std::uint64_t At = 0;
    while (At < Size) {
        const std::int64_t Got = fread(File, At, Bytes.data() + At, Size - At);
        if (Got <= 0) return std::unexpected(Name + " would not read past byte " + std::to_string(At) + Code(Got));
        At += static_cast<std::uint64_t>(Got);
    }
    return Bytes;
}

}  // namespace

std::expected<std::vector<std::uint8_t>, std::string> FontFileRead(std::string_view NameView, std::string_view Tags)
{
    const std::string Name(NameView);
    const std::string Stem = Name.substr(0, Name.rfind('.'));
    if (Stem.empty()) return std::unexpected(Name.empty() ? std::string("no font file is named") :
                                                            "a file called " + Name + " has no stem to be found by");
    if (Stem.find_first_of(":,") != std::string::npos)
        return std::unexpected("a file called " + Name + " cannot be asked for by its name");

    std::string Asked = Stem;
    if (!Tags.empty()) Asked += ", " + std::string(Tags);
    file_id_t *Ids   = nullptr;
    const int  Count = query_all_everywhere(Asked.c_str(), nullptr, &Ids);
    if (Count < 0) return std::unexpected("the volume would not say which files are called " + Name + Code(Count));
    file_id_t First   = 0;
    int       Matches = 0;
    for (int Index = 0; Index < Count; Index++) {
        if (!Called(Ids[Index], Name)) continue;
        if (Matches++ == 0 || Ids[Index] < First) First = Ids[Index];
    }
    std::free(Ids);

    if (Matches == 0)
        return std::unexpected(Tags.empty() ? "no file is called " + Name
                                            : "no file called " + Name + " wears " + std::string(Tags));
    if (Matches > 1)
        kdbg_print("[display] %d files are called %s%s%s; #%llu, the first made, is read", Matches, Name.c_str(),
                   Tags.empty() ? "" : " and wear ", std::string(Tags).c_str(), static_cast<unsigned long long>(First));
    return ReadWhole(First, Name);
}
