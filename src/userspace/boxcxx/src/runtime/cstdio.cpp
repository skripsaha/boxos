
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <print>

#include <__bits/c_file>

#include "box/current.h"
#include "box/file.h"

namespace {

using namespace ::std::__stdio;

::Current *Cur(::std::FILE *f) { return static_cast<::Current *>(f->__cur); }

struct Node {
    ::std::FILE *f;
    char        *tmpname;
    Node        *next;
};
Node       *g_open = nullptr;
::umutex_t  g_open_lock = UMUTEX_INIT;

void Track(::std::FILE *f)
{
    Node *n = static_cast<Node *>(::std::malloc(sizeof(Node)));
    if (!n) return;
    n->f       = f;
    n->tmpname = nullptr;
    ::umutex_lock(&g_open_lock);
    n->next = g_open;
    g_open  = n;
    ::umutex_unlock(&g_open_lock);
}

char *Untrack(::std::FILE *f)
{
    char *name = nullptr;
    ::umutex_lock(&g_open_lock);
    Node **p = &g_open;
    while (*p) {
        if ((*p)->f == f) {
            Node *dead = *p;
            *p   = dead->next;
            name = dead->tmpname;
            ::std::free(dead);
            break;
        }
        p = &(*p)->next;
    }
    ::umutex_unlock(&g_open_lock);
    return name;
}

void MarkTemp(::std::FILE *f, char *owned_name)
{
    ::umutex_lock(&g_open_lock);
    for (Node *n = g_open; n; n = n->next)
        if (n->f == f) { n->tmpname = owned_name; owned_name = nullptr; break; }
    ::umutex_unlock(&g_open_lock);
    ::std::free(owned_name);
}


int RawWrite(::std::FILE *f, const char *p, ::std::size_t n)
{
    if (f->__flags & kConsole) {
        ::std::__print::ToConsole(::std::string_view(p, n));
        return static_cast<int>(n);
    }
    ::Current *c = Cur(f);
    if (!c) return -1;
    return ::current_write(c, p, n);
}

int RawRead(::std::FILE *f, char *p, ::std::size_t n)
{
    ::Current *c = Cur(f);
    if (!c) return -1;
    return ::current_read(c, p, n);
}


bool EnsureBuf(::std::FILE *f)
{
    if (f->__buf) return true;
    const ::std::size_t cap = (f->__mode == _IONBF) ? 1u : BUFSIZ;
    f->__buf = static_cast<char *>(::std::malloc(cap));
    if (!f->__buf) { f->__flags |= kErr; return false; }
    f->__cap    = cap;
    f->__flags |= kOwnBuf;
    return true;
}


void DropReadAhead(::std::FILE *f)
{
    if (!(f->__flags & kReading)) return;
    const ::std::size_t unread = f->__end - f->__pos;
    if (unread && (f->__flags & kSeekable)) {
        ::Current *c = Cur(f);
        if (c) ::current_seek(c, static_cast<::std::uint64_t>(f->__off));
    }
    f->__pos = f->__end = 0;
    f->__flags &= ~kReading;
}

int Fill(::std::FILE *f)
{
    if (!EnsureBuf(f)) return -1;
    if (f->__flags & kWriting) { if (FlushLocked(f) != 0) return -1; f->__flags &= ~kWriting; }

    f->__pos = f->__end = 0;
    const ::std::size_t room = (f->__flags & kKeyboard) ? f->__cap - 1 : f->__cap;
    const int got = RawRead(f, f->__buf, room);
    if (got < 0) { f->__flags |= kErr; return -1; }
    if (got == CURRENT_CLOSED && !(f->__flags & kKeyboard)) {
        f->__flags |= kEof;
        return 0;
    }
    ::std::size_t n = static_cast<::std::size_t>(got);
    if (f->__flags & kKeyboard) f->__buf[n++] = '\n';
    f->__end    = n;
    f->__flags |= kReading;
    return static_cast<int>(n);
}




::std::FILE g_in{};
::std::FILE g_out{};
::std::FILE g_err{};

::std::FILE *InitStd(::std::FILE *f, unsigned flags, int mode)
{
    if (f->__flags) return f;
    f->__unget = EOF;
    f->__mode  = mode;
    f->__flags = flags | kStatic;
    ::umutex_init(&f->__lock);
    if (flags & kKeyboard) f->__cur = ::current_open("keyboard", CURRENT_READ, 0, 0);
    return f;
}

}

namespace std { namespace __stdio {

int FlushLocked(::std::FILE *f)
{
    if (!(f->__flags & kWriting) || f->__pos == 0) return 0;
    const int w = RawWrite(f, f->__buf, f->__pos);
    if (w < 0 || static_cast<::std::size_t>(w) != f->__pos) {
        f->__flags |= kErr;
        return EOF;
    }
    f->__off += static_cast<long long>(f->__pos);
    f->__pos  = 0;
    return 0;
}
int GetLocked(::std::FILE *f)
{
    if (!(f->__flags & kRead)) { f->__flags |= kErr; return EOF; }
    if (f->__unget != EOF) {
        const int c = f->__unget;
        f->__unget = EOF;
        f->__off++;
        return c;
    }
    if (f->__pos >= f->__end) {
        const int got = Fill(f);
        if (got <= 0) return EOF;
    }
    f->__off++;
    return static_cast<unsigned char>(f->__buf[f->__pos++]);
}
int PutLocked(::std::FILE *f, int c)
{
    if (!(f->__flags & kWrite)) { f->__flags |= kErr; return EOF; }
    if (f->__flags & kReading) DropReadAhead(f);
    if (!EnsureBuf(f)) return EOF;
    f->__flags |= kWriting;

    f->__buf[f->__pos++] = static_cast<char>(static_cast<unsigned char>(c));
    const bool full = f->__pos >= f->__cap;
    const bool line = (f->__mode == _IOLBF) && c == '\n';
    if (full || line || f->__mode == _IONBF) {
        if (FlushLocked(f) != 0) return EOF;
    }
    return static_cast<unsigned char>(c);
}

}}

namespace std {

FILE *__stdin() noexcept  { return InitStd(&g_in,  kRead | kKeyboard, _IOLBF); }
FILE *__stdout() noexcept { return InitStd(&g_out, kWrite | kConsole, _IOLBF); }
FILE *__stderr() noexcept { return InitStd(&g_err, kWrite | kConsole, _IONBF); }


FILE *fopen(const char *filename, const char *mode) noexcept
{
    if (!filename || !mode) return nullptr;

    bool read = false, write = false, create = false, trunc = false, append = false;
    switch (mode[0]) {
    case 'r': read = true; break;
    case 'w': write = true; create = true; trunc = true; break;
    case 'a': write = true; create = true; append = true; break;
    default: return nullptr;
    }
    for (const char *p = mode + 1; *p; p++) {
        if (*p == '+') { read = write = true; }
        else if (*p != 'b' && *p != 'x') return nullptr;
        else if (*p == 'x') {
            uint32_t    ids[1];
            file_info_t infos[1];
            if (::find_file_by_name(filename, ids, infos, 1) > 0) return nullptr;
        }
    }

    uint32_t role = 0;
    if (read)  role |= CURRENT_READ;
    if (write) role |= CURRENT_WRITE;
    uint32_t flags = 0;
    if (create) flags |= CURRENT_CREATE;
    if (trunc)  flags |= CURRENT_TRUNCATE;

    const size_t nlen = ::std::strlen(filename);
    char *tag = static_cast<char *>(::std::malloc(nlen + 6));
    if (!tag) return nullptr;
    ::std::memcpy(tag, "file:", 5);
    ::std::memcpy(tag + 5, filename, nlen + 1);
    ::Current *c = ::current_open(tag, role, 0, flags);
    ::std::free(tag);
    if (!c) return nullptr;

    FILE *f = static_cast<FILE *>(::std::malloc(sizeof(FILE)));
    if (!f) { ::current_release(c); return nullptr; }
    *f = FILE{};
    f->__cur   = c;
    f->__unget = EOF;
    f->__mode  = _IOFBF;
    f->__flags = (read ? kRead : 0u) | (write ? kWrite : 0u) | (append ? kAppend : 0u);
    if (::current_caps(c) & CURRENT_CAP_SEEKABLE) f->__flags |= kSeekable;
    ::umutex_init(&f->__lock);

    if (append) {
        const int64_t sz = ::current_size(c);
        if (sz > 0) {
            f->__off = sz;
            ::current_seek(c, static_cast<uint64_t>(sz));
        }
    }
    Track(f);
    return f;
}

int fclose(FILE *stream) noexcept
{
    if (!stream) return EOF;
    int rc = 0;
    {
        Guard g(stream);
        if (FlushLocked(stream) != 0) rc = EOF;
        if (::Current *c = Cur(stream)) {
            ::current_flush(c);
            ::current_release(c);
        }
        stream->__cur = nullptr;
        if (stream->__flags & kOwnBuf) ::std::free(stream->__buf);
        stream->__buf = nullptr;
    }
    if (stream->__flags & kStatic) { stream->__flags &= ~(kWriting | kReading); return rc; }
    char *tmp = Untrack(stream);
    ::std::free(stream);
    if (tmp) { remove(tmp); ::std::free(tmp); }
    return rc;
}

int fflush(FILE *stream) noexcept
{
    if (stream) {
        Guard g(stream);
        const int rc = FlushLocked(stream);
        if (::Current *c = Cur(stream)) ::current_flush(c);
        return rc;
    }
    int rc = 0;
    ::umutex_lock(&g_open_lock);
    for (Node *n = g_open; n; n = n->next) {
        Guard g(n->f);
        if (FlushLocked(n->f) != 0) rc = EOF;
    }
    ::umutex_unlock(&g_open_lock);
    for (FILE *s : {&g_out, &g_err}) {
        if (!s->__flags) continue;
        Guard g(s);
        if (FlushLocked(s) != 0) rc = EOF;
    }
    return rc;
}

FILE *freopen(const char *filename, const char *mode, FILE *stream) noexcept
{
    if (!stream) return nullptr;
    {
        Guard g(stream);
        FlushLocked(stream);
        if (::Current *c = Cur(stream)) { ::current_flush(c); ::current_release(c); }
        stream->__cur = nullptr;
    }
    if (!filename) return nullptr;
    FILE *fresh = fopen(filename, mode);
    if (!fresh) return nullptr;
    Guard g(stream);
    const unsigned keep = stream->__flags & kStatic;
    if (stream->__flags & kOwnBuf) ::std::free(stream->__buf);
    stream->__cur   = fresh->__cur;
    stream->__buf   = nullptr;
    stream->__cap   = stream->__pos = stream->__end = 0;
    stream->__off   = fresh->__off;
    stream->__mode  = fresh->__mode;
    stream->__unget = EOF;
    stream->__flags = (fresh->__flags & ~kStatic) | keep;
    if (char *t = Untrack(fresh)) ::std::free(t);
    ::std::free(fresh);
    return stream;
}

int setvbuf(FILE *stream, char *buf, int mode, size_t size) noexcept
{
    if (!stream) return -1;
    if (mode != _IOFBF && mode != _IOLBF && mode != _IONBF) return -1;
    Guard g(stream);
    if (stream->__pos != 0 || stream->__end != 0) return -1;
    if (stream->__flags & kOwnBuf) { ::std::free(stream->__buf); stream->__flags &= ~kOwnBuf; }
    stream->__buf = nullptr;
    stream->__cap = 0;
    stream->__mode = mode;
    if (buf && size > 0 && mode != _IONBF) {
        stream->__buf = buf;
        stream->__cap = size;
    }
    return 0;
}

void setbuf(FILE *stream, char *buf) noexcept
{
    setvbuf(stream, buf, buf ? _IOFBF : _IONBF, buf ? BUFSIZ : 0);
}


int fgetc(FILE *stream) noexcept
{
    if (!stream) return EOF;
    Guard g(stream);
    if (!ClaimByte(stream)) { stream->__flags |= kErr; return EOF; }
    return GetLocked(stream);
}

int fputc(int c, FILE *stream) noexcept
{
    if (!stream) return EOF;
    Guard g(stream);
    if (!ClaimByte(stream)) { stream->__flags |= kErr; return EOF; }
    return PutLocked(stream, c);
}

int getc(FILE *stream) noexcept { return fgetc(stream); }
int putc(int c, FILE *stream) noexcept { return fputc(c, stream); }
int putchar(int c) noexcept { return fputc(c, __stdout()); }

int ungetc(int c, FILE *stream) noexcept
{
    if (!stream || c == EOF) return EOF;
    Guard g(stream);
    if (!ClaimByte(stream)) { stream->__flags |= kErr; return EOF; }
    if (stream->__unget != EOF) return EOF;
    stream->__unget = static_cast<unsigned char>(c);
    if (stream->__off > 0) stream->__off--;
    stream->__flags &= ~kEof;
    return static_cast<unsigned char>(c);
}

char *fgets(char *s, int n, FILE *stream) noexcept
{
    if (!s || n <= 0 || !stream) return nullptr;
    Guard g(stream);
    if (!ClaimByte(stream)) { stream->__flags |= kErr; return nullptr; }
    int i = 0;
    while (i < n - 1) {
        const int c = GetLocked(stream);
        if (c == EOF) break;
        s[i++] = static_cast<char>(c);
        if (c == '\n') break;
    }
    if (i == 0) return nullptr;
    s[i] = '\0';
    return s;
}

int fputs(const char *s, FILE *stream) noexcept
{
    if (!s || !stream) return EOF;
    Guard g(stream);
    if (!ClaimByte(stream)) { stream->__flags |= kErr; return EOF; }
    for (const char *p = s; *p; p++)
        if (PutLocked(stream, static_cast<unsigned char>(*p)) == EOF) return EOF;
    return 0;
}

int puts(const char *s) noexcept
{
    if (!s) return EOF;
    FILE *f = __stdout();
    Guard g(f);
    if (!ClaimByte(f)) { f->__flags |= kErr; return EOF; }
    for (const char *p = s; *p; p++)
        if (PutLocked(f, static_cast<unsigned char>(*p)) == EOF) return EOF;
    return PutLocked(f, '\n') == EOF ? EOF : 0;
}


size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream) noexcept
{
    if (!ptr || !stream || size == 0 || nmemb == 0) return 0;
    Guard g(stream);
    if (!ClaimByte(stream)) { stream->__flags |= kErr; return 0; }
    char  *out  = static_cast<char *>(ptr);
    size_t done = 0;
    for (; done < nmemb; done++) {
        for (size_t b = 0; b < size; b++) {
            const int c = GetLocked(stream);
            if (c == EOF) return done;
            *out++ = static_cast<char>(c);
        }
    }
    return done;
}

size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream) noexcept
{
    if (!ptr || !stream || size == 0 || nmemb == 0) return 0;
    Guard g(stream);
    if (!ClaimByte(stream)) { stream->__flags |= kErr; return 0; }
    const char *in   = static_cast<const char *>(ptr);
    size_t      done = 0;
    for (; done < nmemb; done++) {
        for (size_t b = 0; b < size; b++) {
            if (PutLocked(stream, static_cast<unsigned char>(*in++)) == EOF) return done;
        }
    }
    return done;
}


int fseek(FILE *stream, long offset, int whence) noexcept
{
    if (!stream) return -1;
    Guard g(stream);
    if (!(stream->__flags & kSeekable)) return -1;
    if (FlushLocked(stream) != 0) return -1;
    stream->__flags &= ~kWriting;
    DropReadAhead(stream);

    long long base = 0;
    switch (whence) {
    case SEEK_SET: base = 0; break;
    case SEEK_CUR: base = stream->__off; break;
    case SEEK_END: {
        const int64_t sz = ::current_size(Cur(stream));
        if (sz < 0) return -1;
        base = sz;
        break;
    }
    default: return -1;
    }
    const long long target = base + offset;
    if (target < 0) return -1;
    if (::current_seek(Cur(stream), static_cast<uint64_t>(target)) != 0) return -1;
    stream->__off   = target;
    stream->__unget = EOF;
    stream->__flags &= ~kEof;
    return 0;
}

long ftell(FILE *stream) noexcept
{
    if (!stream) return -1;
    Guard g(stream);
    if (!(stream->__flags & kSeekable)) return -1;
    return static_cast<long>(stream->__off);
}

void rewind(FILE *stream) noexcept
{
    if (!stream) return;
    fseek(stream, 0, SEEK_SET);
    Guard g(stream);
    stream->__flags &= ~kErr;
}

int fgetpos(FILE *stream, fpos_t *pos) noexcept
{
    if (!stream || !pos) return -1;
    Guard g(stream);
    if (!(stream->__flags & kSeekable)) return -1;
    pos->__off = stream->__off;
    return 0;
}

int fsetpos(FILE *stream, const fpos_t *pos) noexcept
{
    if (!stream || !pos) return -1;
    return fseek(stream, static_cast<long>(pos->__off), SEEK_SET);
}


void clearerr(FILE *stream) noexcept
{
    if (!stream) return;
    Guard g(stream);
    stream->__flags &= ~(kEof | kErr);
}

int feof(FILE *stream) noexcept
{
    if (!stream) return 0;
    Guard g(stream);
    return (stream->__flags & kEof) ? 1 : 0;
}

int ferror(FILE *stream) noexcept
{
    if (!stream) return 0;
    Guard g(stream);
    return (stream->__flags & kErr) ? 1 : 0;
}

void perror(const char *s) noexcept
{
    FILE *e = __stderr();
    if (s && *s) { fputs(s, e); fputs(": ", e); }
    fputs(::std::strerror(errno), e);
    fputc('\n', e);
}


int remove(const char *filename) noexcept
{
    if (!filename) return -1;
    uint32_t    ids[1];
    file_info_t infos[1];
    if (::find_file_by_name(filename, ids, infos, 1) <= 0) return -1;
    return ::file_delete(ids[0]) == 0 ? 0 : -1;
}

int rename(const char *old_p, const char *new_p) noexcept
{
    if (!old_p || !new_p) return -1;
    uint32_t    ids[1];
    file_info_t infos[1];
    if (::find_file_by_name(old_p, ids, infos, 1) <= 0) return -1;
    return ::file_rename(ids[0], new_p) == 0 ? 0 : -1;
}

char *tmpnam(char *s) noexcept
{
    static thread_local char buf[L_tmpnam];
    static unsigned long long counter = 0;

    char *out = s ? s : buf;
    for (unsigned tries = 0; tries < TMP_MAX; tries++) {
        unsigned long long id = ++counter;
        char *p = out;
        *p++ = 't'; *p++ = 'm'; *p++ = 'p';
        char digits[20];
        int  nd = 0;
        do { digits[nd++] = static_cast<char>('0' + id % 10); id /= 10; } while (id);
        while (nd) *p++ = digits[--nd];
        *p = '\0';
        uint32_t    ids[1];
        file_info_t infos[1];
        if (::find_file_by_name(out, ids, infos, 1) <= 0) return out;
    }
    return nullptr;
}

FILE *tmpfile() noexcept
{
    char name[L_tmpnam];
    if (!tmpnam(name)) return nullptr;
    FILE *f = fopen(name, "w+");
    if (!f) return nullptr;
    const size_t n = ::std::strlen(name);
    char *owned = static_cast<char *>(::std::malloc(n + 1));
    if (owned) ::std::memcpy(owned, name, n + 1);
    MarkTemp(f, owned);
    return f;
}

}

extern "C" int getchar(void) { return ::std::fgetc(::std::__stdin()); }