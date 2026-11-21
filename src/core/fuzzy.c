// fuzzy.c - fzf-style fuzzy matching for search as you type.
//
// The pattern's characters must appear in the text in order (case-insensitive; spaces in the
// pattern are ignored). Among all ways to place them, the best-scoring one wins:
//   +16 per matched character
//   +12 when it's the text's first character, +10 at the start of a word, +8 at a camelCase hump
//   + 6 when it directly follows the previous match
//   - 1 per skipped character between matches
// Plus a bonus when the pattern appears as one contiguous piece, and a small penalty for long
// texts so "Details" beats "Details! Mythic+ Scoreboard" for "details".
//
// O(pattern x text) with a running maximum; texts are capped at FUZZY_MAX_TEXT bytes and
// patterns at FUZZY_MAX_PAT, so a few hundred candidates per keystroke cost microseconds.

#define FUZZY_MAX_PAT  32
#define FUZZY_MAX_TEXT 128

typedef struct FuzzyMatch { S32 score; U32 count; U8 pos[FUZZY_MAX_PAT]; } FuzzyMatch; // score < 0: no match

static U8 fz_lower(U8 c) { return c >= 'A' && c <= 'Z' ? (U8)(c + 32) : c; }
static B32 fz_alnum(U8 c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c >= 0x80; }

static S32 fz_bonus(String8 t, U64 j)
{
    if (j == 0) return 12;
    U8 prev = t.str[j - 1], c = t.str[j];
    if (!fz_alnum(prev) && fz_alnum(c)) return 10;                       // "Deadly Boss" -> B
    if (prev >= 'a' && prev <= 'z' && c >= 'A' && c <= 'Z') return 8;    // "HandyNotes" -> N
    if (!(prev >= '0' && prev <= '9') && c >= '0' && c <= '9') return 6;  // "Grid2" -> 2
    return 0;
}

static FuzzyMatch fuzzy_match(String8 pattern, String8 text)
{
    FuzzyMatch r = { -1, 0, { 0 } };
    U8 pat[FUZZY_MAX_PAT];
    U32 m = 0;
    for (U64 i = 0; i < pattern.size && m < FUZZY_MAX_PAT; i += 1) if (pattern.str[i] != ' ') pat[m++] = fz_lower(pattern.str[i]);
    U64 n = Min(text.size, FUZZY_MAX_TEXT);
    if (!m || n < m) return r;
    // Quick reject (most texts, when matching against thousands): not even a subsequence.
    U32 seen = 0;
    for (U64 j = 0; j < n && seen < m; j += 1) seen += fz_lower(text.str[j]) == pat[seen];
    if (seen < m) return r;

    enum { NEG = -30000, MATCH = 16, CONSEC = 6, GAP = 1 };
    thread_static S16 M[FUZZY_MAX_PAT][FUZZY_MAX_TEXT]; // pattern[i] matched exactly at j
    thread_static U8 from_consec[FUZZY_MAX_PAT][FUZZY_MAX_TEXT];
    for (U32 i = 0; i < m; i += 1) {
        S32 best_prev = NEG; // best M[i-1][k] - GAP * (skipped chars) over k < j
        for (U64 j = 0; j < n; j += 1) {
            if (i > 0 && j > 0) {
                best_prev = best_prev > NEG ? best_prev - GAP : NEG;
                if (M[i - 1][j - 1] > best_prev) best_prev = M[i - 1][j - 1];
            }
            S32 score = NEG;
            U8 consec = 0;
            if (fz_lower(text.str[j]) == pat[i]) {
                S32 base = MATCH + fz_bonus(text, j);
                if (i == 0) score = base;
                else if (j > 0) {
                    S32 c = M[i - 1][j - 1] > NEG ? M[i - 1][j - 1] + CONSEC : NEG;
                    S32 g = best_prev > NEG ? best_prev : NEG;
                    if (c >= g && c > NEG) { score = base + c; consec = 1; }
                    else if (g > NEG) score = base + g;
                }
            }
            M[i][j] = (S16)Max(score, NEG);
            from_consec[i][j] = consec;
        }
    }

    // Best end position for the last pattern character, then walk back.
    S32 best = NEG;
    U64 end = 0;
    for (U64 j = 0; j < n; j += 1) if (M[m - 1][j] > best) { best = M[m - 1][j]; end = j; }
    if (best <= NEG) return r;
    U64 j = end;
    for (S32 i = (S32)m - 1; i >= 0; i -= 1) {
        r.pos[i] = (U8)j;
        if (i == 0) break;
        if (from_consec[i][j]) { j -= 1; continue; }
        // The gap predecessor: the k < j maximizing M[i-1][k] - GAP * (j - 1 - k).
        S32 bk = NEG; U64 kk = 0;
        for (U64 k = 0; k < j; k += 1) {
            if (M[i - 1][k] <= NEG) continue;
            S32 v = M[i - 1][k] - GAP * (S32)(j - 1 - k);
            if (v >= bk) { bk = v; kk = k; }
        }
        j = kk;
    }
    r.count = m;

    // Whole pattern as one piece somewhere in the text: likely what was meant.
    B32 contiguous = 1;
    for (U32 i = 1; i < m; i += 1) contiguous &= r.pos[i] == r.pos[i - 1] + 1;
    r.score = best + (contiguous ? 20 : 0) - (S32)(text.size / 8);
    if (r.score < 0) r.score = 0;
    return r;
}
