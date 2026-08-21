#ifndef BOX_STRING_H
#define BOX_STRING_H

#ifdef __cplusplus
extern "C" {
#endif

#include "box/defs.h"

/* The C string primitives. Ф41 completed the set against [cstring.syn] so that
 * boxcxx's <cstring> is a header rather than a second implementation — every
 * name below is the ONE definition in the system, shared by C and C++.
 *
 * All of them are NULL-TOLERANT, which C leaves undefined: strlen(NULL) is 0,
 * a copy into NULL returns NULL, a search in NULL finds nothing. That was
 * already true of the nine that existed before Ф41 and the thirteen new ones
 * match it. A bare-metal system has no signal to turn a null dereference into
 * a diagnostic, so the deviation costs nothing and removes a class of crash.
 * The C locale is the only one BoxOS has, which is why strcoll is strcmp and
 * strxfrm is a copy. */

size_t strlen(const char* str);
char* strcpy(char* dest, const char* src);
char* strncpy(char* dest, const char* src, size_t n);
char* strcat(char* dest, const char* src);
char* strncat(char* dest, const char* src, size_t n);
int strcmp(const char* s1, const char* s2);
int strncmp(const char* s1, const char* s2, size_t n);
int strcoll(const char* s1, const char* s2);
size_t strxfrm(char* dest, const char* src, size_t n);

char* strchr(const char* s, int c);
char* strrchr(const char* s, int c);
char* strstr(const char* haystack, const char* needle);
char* strpbrk(const char* s, const char* accept);
size_t strspn(const char* s, const char* accept);
size_t strcspn(const char* s, const char* reject);

/* Per-STRAND cursor, not the process-wide one C describes: two strands
 * tokenizing two strings must not steal each other's place. The standard
 * leaves strtok's state unspecified and every hosted libc makes it a data
 * race; this one cannot. */
char* strtok(char* s, const char* delim);

void* memcpy(void* dest, const void* src, size_t n);
void* memmove(void* dest, const void* src, size_t n);
void* memset(void* ptr, int value, size_t n);
int memcmp(const void* s1, const void* s2, size_t n);
void* memchr(const void* s, int c, size_t n);

/* C23. memccpy stops after copying the first byte equal to c, which is what
 * makes it the one mem-copy that can be used to append a delimited record
 * without walking the source twice. memset_explicit is memset that the
 * optimizer may not elide: it exists so that erasing a key is an erase and not
 * a dead store. Both are freestanding entities of [cstring.syn], which is why
 * they arrived with the freestanding audit rather than with a caller. */
void* memccpy(void* dest, const void* src, int c, size_t n);
void* memset_explicit(void* ptr, int value, size_t n);

#ifdef __cplusplus
}
#endif

#endif // BOX_STRING_H
