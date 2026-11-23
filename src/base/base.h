// base.h - types, arenas, strings. No platform headers in here.
#ifndef BASE_H
#define BASE_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

typedef uint8_t  U8;
typedef uint16_t U16;
typedef uint32_t U32;
typedef uint64_t U64;
typedef int8_t   S8;
typedef int16_t  S16;
typedef int32_t  S32;
typedef int64_t  S64;
typedef float    F32;
typedef double   F64;
typedef S32      B32;

typedef struct V4 { F32 x, y, z, w; } V4;
typedef struct Rect { F32 x0, y0, x1, y1; } Rect; // pixels: x1, y1 exclusive

#define KB(n) ((U64)(n) << 10)
#define MB(n) ((U64)(n) << 20)
#define GB(n) ((U64)(n) << 30)
#define ArrayCount(a) (sizeof(a) / sizeof((a)[0]))
#define Min(a, b) (((a) < (b)) ? (a) : (b))
#define Max(a, b) (((a) > (b)) ? (a) : (b))
#define Clamp(lo, x, hi) Min(Max(lo, x), hi)
#define AlignPow2(x, a) (((x) + (a) - 1) & ~((U64)(a) - 1))
#define thread_static __declspec(thread) static

//- Arena: reserve a big range of address space, commit as we go, free everything at once.
typedef struct Arena { U8 *base; U64 reserved; U64 committed; U64 pos; } Arena;
typedef struct Temp  { Arena *arena; U64 pos; } Temp;

static Arena *arena_alloc(U64 reserve);
static void   arena_release(Arena *a);
static void  *arena_push(Arena *a, U64 size, B32 zero);
static void   arena_pop_to(Arena *a, U64 pos);
static void   arena_clear(Arena *a);
static Temp   temp_begin(Arena *a);
static void   temp_end(Temp t);
// Per-thread scratch arenas. Pass any arena you are returning results into as `conflict`.
static Temp   scratch_begin(Arena *conflict);
#define scratch_end(t) temp_end(t)
static void   scratch_release_thread(void);

#define push_array(a, T, n)    ((T *)arena_push((a), sizeof(T) * (n), 1))
#define push_array_nz(a, T, n) ((T *)arena_push((a), sizeof(T) * (n), 0))
#define push_struct(a, T)      push_array(a, T, 1)

//- Strings: UTF-8, length-prefixed, not necessarily null-terminated.
typedef struct String8  { U8 *str; U64 size; } String8;
typedef struct String16 { U16 *str; U64 size; } String16;
typedef struct String8Node { struct String8Node *next; String8 string; } String8Node;
typedef struct String8List { String8Node *first, *last; U64 count, total_size; } String8List;

#define str8_lit(s) str8((U8 *)(s), sizeof(s) - 1)
#define S8F(s) (int)(s).size, (char *)(s).str   // printf("%.*s", S8F(s))

static String8 str8(U8 *s, U64 n);
static String8 str8_cstring(char *c);
static String8 str8_copy(Arena *a, String8 s);
static String8 str8_pushf(Arena *a, char *fmt, ...);
static String8 str8_prefix(String8 s, U64 n);
static String8 str8_skip(String8 s, U64 n);
static String8 str8_chop(String8 s, U64 n);
static String8 str8_substr(String8 s, U64 lo, U64 hi);
static String8 str8_trim(String8 s);
static String8 str8_lower(Arena *a, String8 s);
static B32     str8_match(String8 a, String8 b, B32 case_insensitive);
static B32     str8_starts_with(String8 s, String8 prefix, B32 case_insensitive);
static B32     str8_ends_with(String8 s, String8 suffix, B32 case_insensitive);
static U64     str8_find(String8 hay, U64 start, String8 needle, B32 case_insensitive); // hay.size if none
static String8 str8_pop_line(String8 *rest);
static void    str8_list_push(Arena *a, String8List *l, String8 s);
static String8 str8_list_join(Arena *a, String8List *l, String8 sep);
static U8      char_lower(U8 c);
static B32     char_is_space(U8 c);

//- Numbers and times for people to read
static String8 str8_count_short(Arena *a, S64 n);  // 1234567 -> "1.2M", 12345 -> "12K", 999 -> "999"
static String8 str8_thousands(Arena *a, S64 n);    // 12345 -> "12,345"
static String8 str8_ago(Arena *a, U64 seconds);    // how long ago: "just now", "5 min ago", "yesterday", "3 months ago"...

//- Paths (Windows separators)
static String8 path_join(Arena *a, String8 dir, String8 name);
static String8 path_dirname(String8 path);
static String8 path_filename(String8 path);
static String8 path_ext(String8 path); // includes the dot
static String8 path_normalize_slashes(Arena *a, String8 path);

//- UTF-8 decoding
typedef struct UnicodeDecode { U32 codepoint; U32 advance; } UnicodeDecode;
static UnicodeDecode utf8_decode(U8 *s, U64 max);

#endif
