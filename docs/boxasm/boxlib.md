# Calling BoxLib from assembly

BoxLib is the C library of BoxOS, and every program `assemble` writes is
linked with it. A BoxASM program calls BoxLib's functions as a C program
does: `extern` the name, put the arguments where the System V AMD64 ABI puts
them, `call`. This is how an assembly program prints with formats, reads and
writes files, waits on Touch, starts strands and other programs — everything
the system offers.

```nasm
section .rodata
fmt: db "%d + %d = %d", 10, 0

section .text
extern printf
global main

main:
    sub rsp, 8          ; rsp to a multiple of 16 for the call
    lea rdi, [fmt]
    mov esi, 2
    mov edx, 3
    mov ecx, 5
    xor eax, eax        ; printf is variadic: al = vector registers used
    call printf
    add rsp, 8
    xor eax, eax
    ret
```

## The call

| | |
|---|---|
| integer and pointer arguments | `rdi rsi rdx rcx r8 r9`, then the stack, right to left |
| floating-point arguments | `xmm0`..`xmm7` |
| the stack at `call` | rsp a multiple of 16 |
| a variadic callee (`printf`, `kdbg_print`) | `al` = how many vector registers carry arguments |
| kept by the callee | `rbx rbp r12 r13 r14 r15`, and rsp |
| may change | `rax rcx rdx rsi rdi r8`–`r11`, every xmm, ymm, mm and x87 register, the flags |
| the value | `rax` (`eax` for `int` and 32-bit types, `al` for `bool`); `xmm0` for floating |

`main` is entered with rsp 8 below a multiple of 16 (the `call` pushed its
return address): one `push` or `sub rsp, 8` makes it a multiple again.

Types: `size_t`, pointers, `int64_t` and `file_id_t` are 8 bytes; `int`,
`uint32_t` and `TouchTag` 4; `bool` 1. Structures are passed by pointer,
laid out by the C rules (each field at a multiple of its size); the layouts a
program needs are below. A few small structures travel in registers:

- `Luggage luggage(void)` returns its bytes in `rax`, its length in `edx`.
- `TouchTagPair touch_intern(const char *tag)` returns `full` in the low 32
  bits of `rax`, `bare` in the high 32; `touch_send` takes the pair the same
  way, in `rdi`.
- `TurnInMark box_mark(void)` returns `rax:rdx`; `box_mark_moved` and
  `box_turn_in` take it in `rdi:rsi`.

### Functions BoxLib calls back

A function whose address is handed to BoxLib — the body of a strand, a heap
iteration callback, a format sink — is reached by an indirect call, and on a
processor with CET that call must land on `endbr64`. BoxASM refuses an
address of code taken as a value unless it begins with one:

```nasm
worker:
    endbr64
    ...
```

## Errors

A BoxLib function that fails returns a negative number, and the error is its
negation: `-300` is `ERR_FILE_NOT_FOUND`. `neg eax` gives the code. There is
no `errno`.

```nasm
    call create             ; rax: the new file's id, or -error
    test rax, rax
    js .failed
```

Not all of BoxLib keeps that rule. These return the code positive, or 0:

| Functions | Return |
|---|---|
| `touch_claim touch_release touch_register touch_ack touch_irq_return`, `bay_release brook_release brook_hand_over` | 0, or a positive or negative error: test for not 0 |
| `time_get time_get_secs time_uptime_ms time_uptime_ns time_format` | 0, or a positive error |
| `addr_park addr_wake heap_get_last_error` | `error_t`: 0, or a positive error |
| `bay_open brook_open current_open` | a pointer, or NULL |
| `strand_spawn strand_spawn_joinable` | a pid, or 0 |
| `readline readline_grow getchar input printf` | -1 on failure |

The codes a program meets most:

| Code | Value | | Code | Value |
|---|---|---|---|---|
| `ERR_INVALID_ARGUMENT` | 3 | | `ERR_FILE_NOT_FOUND` | 300 |
| `ERR_NULL_POINTER` | 4 | | `ERR_ALREADY_EXISTS` | 303 |
| `ERR_OUT_OF_RANGE` | 5 | | `ERR_PROCESS_NOT_FOUND` | 400 |
| `ERR_BUFFER_TOO_SMALL` | 6 | | `ERR_PROCESS_TERMINATED` | 403 |
| `ERR_TIMEOUT` | 7 | | `ERR_ACCESS_DENIED` | 500 |
| `ERR_WOULD_BLOCK` | 9 | | `ERR_PERMISSION_DENIED` | 501 |
| `ERR_NO_MEMORY` | 100 | | `ERR_TAGFS_NO_SPACE` | 852 |
| `ERR_IO` | 200 | | `ERR_TAGFS_BEYOND_SIZE` | 867 |

All of them are in `src/userspace/boxlib/include/box/error.h`.

## A program's start and end

- `_start` calls `main` with rdi and rsi 0. A program has no argc and argv:
  what was typed is read through the Form and the Luggage, below.
- Returning from `main` calls `exit(eax)`: BoxLib's teardown, the output
  flushed, then the program ends — every strand of it.
- `exit(code)` does the same from anywhere. `_Exit(code)` ends at once and
  does not flush what was printed.

## Linking

The linker takes the program, BoxLib's start code, and from `libbox.a` each
member that defines a name the program, or a member already taken, needs.
An `extern` name nothing defines is refused, with the name. The C++ library
is not linked: `clock_boxtime_to_local` and `clock_boxtime_local` are
declared in BoxLib's headers but live there, and are not available.

## The functions

What follows is BoxLib's public surface by subject, the signatures as the
headers write them. The headers in `src/userspace/boxlib/include/box/` are
the authority; each says more of what its functions do.

### Printing and reading the console — `print.h`, `color.h`

```c
int  printf(const char *fmt, ...);        /* C17's conversions and flags, exact floats;
                                             %color and %bgcolor take a Color; -1 on failure */
void print(const char *str);              /* to the screen, or to the file `cmd -> file` names */
void println(const char *str);
void print_bytes(const char *data, size_t len);
void print_to_screen(const char *str);    /* the screen, whatever -> says */
void clear(void);
void io_flush(void);
int  readline(char *buffer, size_t max_len);      /* a line edited; its length, or -1 */
int  readline_grow(char **line);                  /* the line on the heap; the caller frees it */
int  input(const char *prompt, char *buffer, size_t max_len);
int  getchar(void);
void set_color(Color fg);    void set_color_bg(Color bg);    /* Color = 0x00RRGGBB */
```

### The command line and the Form — `form.h`, `luggage.h`

```c
int form_things(uint32_t field, const file_id_t **ids, size_t *count);     /* a thing or things field */
int form_words(uint32_t field, const char *const **words, size_t *count);  /* name, words, text, into */
const char *form_text(uint32_t field);           /* a field's words joined by spaces */
int souvenir_things(const file_id_t *ids, size_t count);   /* the files the program gives back */

uint32_t    luggage_word_count(void);
const char *luggage_word(uint32_t index);        /* word 0 is the program's name; NULL past the end */
const char *luggage_tail(uint32_t from);         /* the rest of the line as typed */
```

Fields are numbered from 0 in the order the `takes` lines stand. The arrays
`form_things` and `form_words` give live as long as the program. A field
left empty gives a count of 0.

### Files and tags — `file.h`, `use.h`

```c
int64_t create(const char *filename, const char *tags);     /* a new file in the Use Context: its id or -error */
int64_t create_everywhere(const char *filename, const char *tags);
int64_t fread(file_id_t file, uint64_t offset, void *buffer, size_t size);         /* bytes, or -error */
int64_t fwrite(file_id_t file, uint64_t offset, const void *buffer, size_t size);  /* within its size */
int file_span(file_id_t file, uint64_t size, uint32_t flags);   /* set the size; STORAGE_SPAN_GROW = 1 */
int FileMetadataRead(file_id_t file, FileMetadata *out);        /* name, size, tags, whole */
void FileMetadataRelease(FileMetadata *metadata);
int file_info(file_id_t file, file_info_t *info);
int query(const char *tags, file_id_t *ids, size_t max);        /* the files wearing every tag: how many */
int query_all(const char *tags, file_id_t **ids);               /* all of them, on the heap */
int tag_add(file_id_t file, const char *tag);
int tag_remove(file_id_t file, const char *key);
int file_rename(file_id_t file, const char *name);
int delete(file_id_t file);
int anchor(file_id_t file);                      /* what was written made durable; 0: the volume */
int use_get(char *buf, size_t cap, size_t *needed);
```

`fwrite` writes within the file's size: `file_span` makes room first. A read
or a write moves at most 4 GiB at a time.

```text
FileMetadata (48 bytes)          file_info_t (184 bytes)
  @0  file_id_t FileId             @0   file_id_t file_id
  @8  uint32_t  Flags              @8   uint32_t  flags
  @16 uint64_t  Size               @16  uint64_t  size
  @24 uint16_t  TagCount           @24  uint8_t   tag_count
  @32 char     *Filename           @25  char      filename[32]
  @40 char     *Tags  (a, b, c)    @57  tag_t     tags[5]  (24 bytes each)
```

### Memory — `memory.h`, `string.h`, `convert.h`

```c
void *malloc(size_t size);           /* 16-byte aligned */
void *calloc(size_t n, size_t size);
void *realloc(void *ptr, size_t size);
void *aligned_alloc(size_t alignment, size_t size);
void  free(void *ptr);
```

`string.h` has C's `strlen strcmp strncmp strcpy strncpy strcat strchr
strrchr strstr strspn strcspn strtok memcpy memmove memset memcmp memchr`;
`convert.h` `to_int to_uint to_int64 hex_to_int to_str to_hex int64_to_str
uint64_to_hex is_digit is_alpha is_space to_upper to_lower`.

### Touch: events — `touch.h`

```c
TouchTagPair touch_intern(const char *tag);       /* a tag's name made its number */
bool touch_wait(Touch *out, uint32_t timeout_ms);               /* the next event; 0 ms: no timeout */
bool touch_wait_tag(TouchTag tag, Touch *out, uint32_t timeout_ms);
bool touch_pop(Touch *out);                                     /* one if there is one, at once */
int  touch_send(TouchTagPair pair, const void *payload, uint32_t length, uint32_t after_ms);
int  touch_await(TouchTag tag, Touch *out, uint32_t timeout_ms);
TouchTag console_listen(void);                    /* the tag this strand's keys come on (print.h) */
```

The system's tags include `keyboard`, `process:died`, `process:spawned`,
`system:shutdown`, `system:reboot`, `usb:arrived`, `usb:left`.

```text
Touch (120 bytes)                  a key (keyboard), at payload
  @0  uint32_t tag_id                @24 uint32_t cp: the character typed, Unicode
  @4  uint16_t flags                     (0 for keys without one)
  @8  uint32_t source_pid            @28 uint8_t scancode
                                     @29 uint8_t mods: shift 1, ctrl 2, alt 4, extended 8
  @12 uint32_t payload_len
  @16 uint64_t timestamp_tsc       process:died, at payload
  @24 uint8_t  payload[96]           @24 uint32_t pid; @28 int32_t exit_code; @32 uint32_t generation
```

### Strands and waiting — `strand.h`, `sync.h`

```c
uint32_t strand_spawn(void (*fn)(void *arg), void *arg);   /* a strand of this program: its pid, 0 on failure */
uint32_t strand_spawn_joinable(void (*fn)(void *arg), void *arg);
void     strand_release(uint32_t pid);
void     strand_exit(void);                                /* this strand ends; returning from fn does too */
error_t  addr_park(const volatile void *addr, uint64_t expected, uint32_t timeout_ms);
                                                           /* sleep while the qword at addr is expected */
error_t  addr_wake(const volatile void *addr, uint32_t count);   /* 0: wake all */
void     yield(void);
```

Strands share the program's memory. The word `addr_park` watches is a qword
on an 8-byte boundary. A strand's function starts with `endbr64`.

### Programs — `system.h`

```c
int  proc_exec(const char *line);                 /* start a program by its line: its pid, or -error */
int  proc_exec_file(file_id_t program, const void *luggage, size_t length,
                    const char *tags, uint32_t *generation);
int  process_gone(uint32_t pid, uint32_t generation, int32_t *out_exit);  /* wait for its end */
int  proc_kill(uint32_t pid);                     /* another program, whole */
void exit(int code);
int  sysinfo(system_info_t *info);
```

### Time — `time.h`, `clock.h`

```c
uint64_t clock_uptime_ms(void);      uint64_t clock_uptime_us(void);
uint64_t clock_unix_now(void);       /* seconds since 1970 */
int      clock_boxtime(BoxTime *out);              /* the date and time, UTC */
int      time_format(const BoxTime *t, char *buf, size_t size);   /* "YYYY-MM-DD HH:MM:SS"; size >= 20 */
```

`BoxTime` (20 bytes, packed): `@0 seconds (u64)`, `@8 nanosec (u32)`, `@12
year (u16)`, `@14 month`, `@15 day`, `@16 hour`, `@17 minute`, `@18
second`, `@19 weekday` (one byte each).

### Streams, shared memory and messages — `current.h`, `brook.h`, `bay.h`, `ipc.h`

`current_open(tag, role, item_size, flags)` opens a Current — the screen,
the keyboard, a log, a file, or a stream between programs — read with
`current_read` and written with `current_write`. `brook_open` makes a stream
of fixed frames between two strands or programs, `bay_open` a block of memory
shared by tag, and `send` and `broadcast` deliver a message to a program or
to every program wearing a tag. Their headers give the roles, flags and
layouts.

### The rest

`debug.h` (`kdbg`, to the kernel's log), `vga.h` (the text screen, cell by
cell), `nameplate.h` (the function an address is in), `memtag.h` and
`pku.h` (memory tags and protection keys), `recipe.h` and `sha256.h`, `hw.h`.
`cpu.h`'s functions are C inline functions, not in the library: in assembly
write `cpuid` and `rdtsc` themselves.
