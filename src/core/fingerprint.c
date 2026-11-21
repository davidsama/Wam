// fingerprint.c - CurseForge-style fingerprints.
//
// CurseForge identifies files by MurmurHash2 (seed 1) of the file bytes with the
// whitespace bytes 9, 10, 13 and 32 removed. A WoW addon folder's fingerprint is
// built from the files its .toc pulls in (see addons.c), following the algorithm
// that open-source managers such as WowUp use (verified against the live API).
//
// Wago uses the same file set with MD5 instead (see md5_hex below).

static U32 murmur2(U8 *data, U64 len, U32 seed)
{
    const U32 m = 0x5bd1e995;
    const int r = 24;
    U32 h = seed ^ (U32)len;
    while (len >= 4) {
        U32 k;
        memcpy(&k, data, 4);
        k *= m; k ^= k >> r; k *= m;
        h *= m; h ^= k;
        data += 4; len -= 4;
    }
    switch (len) {
        case 3: h ^= (U32)data[2] << 16; // fallthrough
        case 2: h ^= (U32)data[1] << 8;  // fallthrough
        case 1: h ^= data[0]; h *= m;
    }
    h ^= h >> 13; h *= m; h ^= h >> 15;
    return h;
}

static B32 cf_is_whitespace(U8 c) { return c == 9 || c == 10 || c == 13 || c == 32; }

static U32 cf_fingerprint(String8 data)
{
    Temp scratch = scratch_begin(0);
    U8 *buf = push_array_nz(scratch.arena, U8, data.size + 1);
    U64 n = 0;
    for (U64 i = 0; i < data.size; i += 1) {
        U8 c = data.str[i];
        if (!cf_is_whitespace(c)) buf[n++] = c;
    }
    U32 h = murmur2(buf, n, 1);
    scratch_end(scratch);
    return h;
}

//- MD5 (RFC 1321), for Wago fingerprints: hex of each file's raw bytes, sorted, concatenated, hashed again.

static void md5_block(U32 st[4], const U8 *p)
{
    static const U32 K[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
        0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
        0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
        0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
    };
    static const U8 R[64] = {
        7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
        4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
    };
    U32 w[16];
    for (int i = 0; i < 16; i += 1) w[i] = (U32)p[i * 4] | (U32)p[i * 4 + 1] << 8 | (U32)p[i * 4 + 2] << 16 | (U32)p[i * 4 + 3] << 24;
    U32 a = st[0], b = st[1], c = st[2], d = st[3];
    for (int i = 0; i < 64; i += 1) {
        U32 f; int g;
        if (i < 16)      { f = (b & c) | (~b & d); g = i; }
        else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) & 15; }
        else if (i < 48) { f = b ^ c ^ d;          g = (3 * i + 5) & 15; }
        else             { f = c ^ (b | ~d);       g = (7 * i) & 15; }
        U32 t = d; d = c; c = b;
        U32 x = a + f + K[i] + w[g];
        b = b + ((x << R[i]) | (x >> (32 - R[i])));
        a = t;
    }
    st[0] += a; st[1] += b; st[2] += c; st[3] += d;
}

// Lowercase hex MD5 of `data` into out[33] (NUL-terminated).
static void md5_hex(String8 data, char out[33])
{
    U32 st[4] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476 };
    U64 full = data.size & ~63ull;
    for (U64 i = 0; i < full; i += 64) md5_block(st, data.str + i);
    U8 tail[128] = { 0 };
    U64 rem = data.size - full;
    if (rem) memcpy(tail, data.str + full, rem);
    tail[rem] = 0x80;
    U64 tail_len = rem < 56 ? 64 : 128;
    U64 bits = data.size * 8;
    for (int i = 0; i < 8; i += 1) tail[tail_len - 8 + i] = (U8)(bits >> (8 * i));
    md5_block(st, tail);
    if (tail_len == 128) md5_block(st, tail + 64);
    static const char hexd[] = "0123456789abcdef";
    for (int i = 0; i < 16; i += 1) {
        U8 byte = (U8)(st[i / 4] >> (8 * (i % 4)));
        out[i * 2] = hexd[byte >> 4];
        out[i * 2 + 1] = hexd[byte & 15];
    }
    out[32] = 0;
}
