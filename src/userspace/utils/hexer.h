#ifndef HEXER_H
#define HEXER_H

/* hexer — a file's bytes on the glass, changed in place and put back.
 *
 * Four parts, as draft is: hexer.cpp decides — the Form, the words, the keys,
 * the word line, opening and leaving; hexer_bytes.cpp holds the file — what is
 * read, what was changed, saving and finding; hexer_paint.cpp turns both into
 * one frame; hexer_list.cpp is the text an arrow takes instead of a frame.
 */

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "box/color.h"
#include "box/file.h"
#include "box/keyboard.h"
#include "box/touch.h"
#include "box/vga.h"

/* What `find` looks for, and whether it was asked as bytes or as text. */
struct HexerNeedle {
    std::vector<std::uint8_t> Bytes;
    bool                      AsBytes = false;
};

/* The part of the file hexer looks at: bytes Lo up to Hi. */
struct HexerPart {
    std::uint64_t Lo = 0;
    std::uint64_t Hi = 0;
};

/* The file as it is now: its bytes, with every change not yet saved laid over
 * them. Only what is looked at is read; a change is kept here until a save
 * puts it back, so a file of any size opens at once. */
class HexerBytes {
public:
    HexerBytes(file_id_t File, HexerPart Part) : File(File), Part(Part) {}

    /* Count bytes from At, changes laid over; Changed[i] is 1 where byte i
     * was changed. Fewer when the file ends sooner or will not be read. */
    std::size_t Read(std::uint64_t At, std::uint8_t *Out, std::size_t Count, std::uint8_t *Changed) const;
    void        Set(std::uint64_t At, std::uint8_t Value) { Changes[At] = Value; }
    std::size_t ChangeCount() const { return Changes.size(); }

    /* Every change into the file, then anchored. The sentence to say. */
    std::string Save();
    std::uint64_t SavedBytes() const { return Saved; }

    /* The next place after From where Needle stands, wrapping once to Lo. */
    std::optional<std::uint64_t> Find(std::span<const std::uint8_t> Needle, std::uint64_t From) const;

private:
    bool FindIn(std::span<const std::uint8_t> Needle, std::uint64_t First, std::uint64_t Last,
                std::uint64_t &Found) const;

    file_id_t                          File;
    HexerPart                          Part;
    std::map<std::uint64_t, std::uint8_t> Changes;
    std::uint64_t                      Saved = 0;
};

class Hexer {
public:
    Hexer(file_id_t File, std::string Name, std::string Tags, HexerPart Part);

    /* The screen and the ear taken; the sentence to say when they cannot be. */
    std::optional<std::string> TakeScreen();
    void Run();
    void GiveScreenBack();
    std::string Summary() const;

private:
    /* hexer.cpp */
    void OnKey(const kb_event_t &Key);
    void OnEditKey(const kb_event_t &Key);
    void OnCommandKey(const kb_event_t &Key);
    void RunCommand();
    void Go(std::string_view Where);
    void Find(std::string_view What);
    void Save();
    void Leave();
    void Type(std::uint8_t Value);
    void MoveTo(std::uint64_t At);
    void MoveBy(std::int64_t Delta);

    /* hexer_paint.cpp */
    void          Paint();
    void          FollowCursor();
    void          PaintTitle();
    void          PaintLines();
    void          PaintFoot();
    void          Put(std::uint32_t Row, std::uint32_t Col, char Ch, Color Fg, Color Bg);
    std::uint32_t Write(std::uint32_t Row, std::uint32_t Col, std::string_view Text, Color Fg, Color Bg);
    std::uint32_t TextRows() const { return Rows - 2; }
    std::uint32_t HexColumn(std::uint32_t Byte) const;
    std::uint32_t TextColumn(std::uint32_t Byte) const;

    HexerBytes  Bytes;
    std::string Name;
    std::string Tags;
    HexerPart   Part;

    std::uint64_t At = 0;           /* the cursor's byte */
    bool          InText = false;   /* typing into the text column, not the hex */
    bool          LowHalf = false;  /* in hex, the next digit sets the low half */
    std::uint64_t Top = 0;          /* the offset of the first line drawn */

    std::uint32_t         Rows = 0;
    std::uint32_t         Cols = 0;
    std::uint32_t         Width = 16;  /* bytes a line shows; 8 on a narrow screen */
    std::uint32_t         Digits = 8;
    std::vector<TextCell> Frame;
    TouchTag              Ear = TOUCH_TAG_INVALID;

    bool                      CommandOpen = false;
    std::string               Command;
    std::string               Message;
    HexerNeedle               LastFind;
    bool                      Leaving = false;
    unsigned                  Saves = 0;
};

/* hexer_list.cpp — under an arrow: the part as text, for the file. False when
 * the file would not be read to the end of the part. */
bool HexerList(file_id_t File, const FileMetadata &Metadata, HexerPart Part);

#endif /* HEXER_H */
