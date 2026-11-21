// zip.c - read-only .zip support: central directory, stored + DEFLATE entries, CRC32.
// The inflater is a compact canonical-Huffman decoder (RFC 1951). Addon zips are
// a few MB at most, so it favors small code over raw speed.

//- CRC32

static U32 crc32_table[256];

static U32 crc32(String8 data)
{
    if (!crc32_table[1]) {
        for (U32 i = 0; i < 256; i += 1) {
            U32 c = i;
            for (int k = 0; k < 8; k += 1) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            crc32_table[i] = c;
        }
    }
    U32 c = 0xFFFFFFFFu;
    for (U64 i = 0; i < data.size; i += 1) c = crc32_table[(c ^ data.str[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

//- Inflate

typedef struct Inflate {
    U8 *in; U64 in_len, in_pos;
    U64 bitbuf; U32 bitcnt;
    U8 *out; U64 out_len, out_pos;
    B32 err;
} Inflate;

#define HUFF_FAST_BITS 10
// fast[] maps the next HUFF_FAST_BITS input bits to (length << 9 | symbol); 0 = use the slow path.
typedef struct Huffman { S16 count[16]; S16 symbol[288]; U16 fast[1 << HUFF_FAST_BITS]; } Huffman;

static U32 inf_bits(Inflate *s, U32 need)
{
    while (s->bitcnt < need) {
        if (s->in_pos >= s->in_len) { s->err = 1; return 0; }
        s->bitbuf |= (U64)s->in[s->in_pos++] << s->bitcnt;
        s->bitcnt += 8;
    }
    U32 v = (U32)(s->bitbuf & ((1ull << need) - 1));
    s->bitbuf >>= need;
    s->bitcnt -= need;
    return v;
}

// Returns 0 if complete, <0 if over-subscribed, >0 if incomplete.
static int huff_build(Huffman *h, U8 *lengths, int n)
{
    memset(h->count, 0, sizeof(h->count));
    for (int i = 0; i < n; i += 1) h->count[lengths[i]] += 1;
    if (h->count[0] == n) return 0;
    int left = 1;
    for (int len = 1; len < 16; len += 1) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) return left;
    }
    S16 offs[16];
    offs[1] = 0;
    for (int len = 1; len < 15; len += 1) offs[len + 1] = (S16)(offs[len] + h->count[len]);
    for (int i = 0; i < n; i += 1) if (lengths[i]) h->symbol[offs[lengths[i]]++] = (S16)i;

    // Canonical codes are assigned in (length, symbol) order, which is the order of symbol[].
    memset(h->fast, 0, sizeof(h->fast));
    int code = 0, index = 0;
    for (int len = 1; len < 16; len += 1) {
        for (int k = 0; k < h->count[len]; k += 1, code += 1) {
            int sym = h->symbol[index++];
            if (len > HUFF_FAST_BITS) continue;
            int rev = 0; // deflate sends codes MSB-first into an LSB-first bit stream
            for (int b = 0; b < len; b += 1) rev |= ((code >> b) & 1) << (len - 1 - b);
            for (int j = rev; j < (1 << HUFF_FAST_BITS); j += 1 << len) h->fast[j] = (U16)((len << 9) | sym);
        }
        code <<= 1;
    }
    return left;
}

static int huff_decode(Inflate *s, Huffman *h)
{
    while (s->bitcnt < HUFF_FAST_BITS && s->in_pos < s->in_len) {
        s->bitbuf |= (U64)s->in[s->in_pos++] << s->bitcnt;
        s->bitcnt += 8;
    }
    if (s->bitcnt >= HUFF_FAST_BITS) {
        U16 e = h->fast[s->bitbuf & ((1u << HUFF_FAST_BITS) - 1)];
        if (e) {
            U32 len = e >> 9;
            s->bitbuf >>= len;
            s->bitcnt -= len;
            return e & 511;
        }
    }
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; len += 1) {
        code |= (int)inf_bits(s, 1);
        if (s->err) return -1;
        int count = h->count[len];
        if (code - count < first) return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

static const U16 len_base[29]  = { 3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258 };
static const U8  len_extra[29] = { 0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0 };
static const U16 dist_base[30] = { 1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577 };
static const U8  dist_extra[30]= { 0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13 };

static void inf_codes(Inflate *s, Huffman *lencode, Huffman *distcode)
{
    for (;;) {
        int sym = huff_decode(s, lencode);
        if (sym < 0) { s->err = 1; return; }
        if (sym < 256) {
            if (s->out_pos >= s->out_len) { s->err = 1; return; }
            s->out[s->out_pos++] = (U8)sym;
            continue;
        }
        if (sym == 256) return;
        sym -= 257;
        if (sym >= 29) { s->err = 1; return; }
        U32 len = len_base[sym] + inf_bits(s, len_extra[sym]);
        int dsym = huff_decode(s, distcode);
        if (dsym < 0 || dsym >= 30) { s->err = 1; return; }
        U64 dist = dist_base[dsym] + inf_bits(s, dist_extra[dsym]);
        if (s->err || dist > s->out_pos || s->out_pos + len > s->out_len) { s->err = 1; return; }
        U8 *dst = s->out + s->out_pos, *src = dst - dist;
        for (U32 i = 0; i < len; i += 1) dst[i] = src[i]; // may overlap: byte by byte on purpose
        s->out_pos += len;
    }
}

static void inf_stored(Inflate *s)
{
    // Stored blocks start on a byte boundary: drop the partial byte, give back whole buffered bytes.
    s->in_pos -= s->bitcnt / 8;
    s->bitbuf = 0;
    s->bitcnt = 0;
    if (s->in_pos + 4 > s->in_len) { s->err = 1; return; }
    U32 len = s->in[s->in_pos] | (s->in[s->in_pos + 1] << 8);
    U32 nlen = s->in[s->in_pos + 2] | (s->in[s->in_pos + 3] << 8);
    s->in_pos += 4;
    if (len != (~nlen & 0xFFFF) || s->in_pos + len > s->in_len || s->out_pos + len > s->out_len) { s->err = 1; return; }
    memcpy(s->out + s->out_pos, s->in + s->in_pos, len);
    s->in_pos += len;
    s->out_pos += len;
}

static Huffman inf_fixed_len, inf_fixed_dist;
static B32 inf_fixed_built;

// Builds the shared tables. Call once before inflating on several threads.
static void inflate_init(void)
{
    crc32(str8(0, 0));
    if (!inf_fixed_built) {
        U8 lengths[288];
        int i = 0;
        for (; i < 144; i += 1) lengths[i] = 8;
        for (; i < 256; i += 1) lengths[i] = 9;
        for (; i < 280; i += 1) lengths[i] = 7;
        for (; i < 288; i += 1) lengths[i] = 8;
        huff_build(&inf_fixed_len, lengths, 288);
        for (i = 0; i < 30; i += 1) lengths[i] = 5;
        huff_build(&inf_fixed_dist, lengths, 30);
        inf_fixed_built = 1;
    }
}

static void inf_fixed(Inflate *s)
{
    inflate_init();
    inf_codes(s, &inf_fixed_len, &inf_fixed_dist);
}

static void inf_dynamic(Inflate *s)
{
    static const U8 order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
    U8 lengths[320];
    Huffman lencode, distcode;
    int nlen = (int)inf_bits(s, 5) + 257;
    int ndist = (int)inf_bits(s, 5) + 1;
    int ncode = (int)inf_bits(s, 4) + 4;
    if (s->err || nlen > 286 || ndist > 30) { s->err = 1; return; }

    memset(lengths, 0, sizeof(lengths));
    for (int i = 0; i < ncode; i += 1) lengths[order[i]] = (U8)inf_bits(s, 3);
    if (huff_build(&lencode, lengths, 19) != 0) { s->err = 1; return; }

    int index = 0;
    while (index < nlen + ndist) {
        int sym = huff_decode(s, &lencode);
        if (sym < 0 || s->err) { s->err = 1; return; }
        if (sym < 16) { lengths[index++] = (U8)sym; continue; }
        U8 len = 0;
        int repeat;
        if (sym == 16) {
            if (index == 0) { s->err = 1; return; }
            len = lengths[index - 1];
            repeat = 3 + (int)inf_bits(s, 2);
        } else if (sym == 17) {
            repeat = 3 + (int)inf_bits(s, 3);
        } else {
            repeat = 11 + (int)inf_bits(s, 7);
        }
        if (index + repeat > nlen + ndist) { s->err = 1; return; }
        while (repeat--) lengths[index++] = len;
    }
    if (lengths[256] == 0) { s->err = 1; return; }

    int err = huff_build(&lencode, lengths, nlen);
    if (err < 0 || (err > 0 && nlen - lencode.count[0] != 1)) { s->err = 1; return; }
    err = huff_build(&distcode, lengths + nlen, ndist);
    if (err < 0 || (err > 0 && ndist - distcode.count[0] != 1)) { s->err = 1; return; }
    inf_codes(s, &lencode, &distcode);
}

// Inflates raw DEFLATE data into a buffer of exactly out_len bytes.
static B32 inflate_raw(String8 in, U8 *out, U64 out_len)
{
    Inflate s = { 0 };
    s.in = in.str; s.in_len = in.size;
    s.out = out; s.out_len = out_len;
    U32 last;
    do {
        last = inf_bits(&s, 1);
        U32 type = inf_bits(&s, 2);
        if (s.err) break;
        if (type == 0) inf_stored(&s);
        else if (type == 1) inf_fixed(&s);
        else if (type == 2) inf_dynamic(&s);
        else s.err = 1;
    } while (!last && !s.err);
    return !s.err && s.out_pos == out_len;
}

//- Zip archive

typedef struct ZipEntry {
    String8 name;  // as stored, '/' separators
    U32 method, crc, comp_size, size, local_offset;
    B32 is_dir;
} ZipEntry;

typedef struct ZipArchive { String8 data; ZipEntry *v; U64 count; } ZipArchive;

static U32 rd16(U8 *p) { return p[0] | (p[1] << 8); }
static U32 rd32(U8 *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((U32)p[3] << 24); }

// Finds the end-of-central-directory record in the last bytes of a zip (tail = up to the
// final 64 KB + 22 bytes of the file). Outputs entry count, directory size and offset.
static B32 zip_find_eocd(String8 tail, U32 *total, U32 *cd_size, U32 *cd_off)
{
    if (tail.size < 22) return 0;
    U64 lowest = tail.size > 22 + 65535 ? tail.size - 22 - 65535 : 0;
    for (U64 i = tail.size - 22 + 1; i-- > lowest;) {
        if (rd32(tail.str + i) == 0x06054b50) {
            *total = rd16(tail.str + i + 10);
            *cd_size = rd32(tail.str + i + 12);
            *cd_off = rd32(tail.str + i + 16);
            return 1;
        }
    }
    return 0;
}

// Parses central directory records (cd = the directory bytes only).
static B32 zip_parse_central(Arena *a, String8 cd, U32 total, ZipArchive *out)
{
    out->v = push_array(a, ZipEntry, total);
    out->count = 0;
    U8 *p = cd.str, *end = cd.str + cd.size;
    for (U32 i = 0; i < total; i += 1) {
        if (end - p < 46 || rd32(p) != 0x02014b50) return 0;
        U32 name_len = rd16(p + 28), extra_len = rd16(p + 30), comment_len = rd16(p + 32);
        if ((U64)(end - p) < 46ull + name_len + extra_len + comment_len) return 0;
        ZipEntry *e = &out->v[out->count++];
        e->method = rd16(p + 10);
        e->crc = rd32(p + 16);
        e->comp_size = rd32(p + 20);
        e->size = rd32(p + 24);
        e->local_offset = rd32(p + 42);
        e->name = str8(p + 46, name_len);
        e->is_dir = name_len && (p[46 + name_len - 1] == '/' || p[46 + name_len - 1] == '\\');
        p += 46 + name_len + extra_len + comment_len;
    }
    return 1;
}

static B32 zip_open(Arena *a, String8 data, ZipArchive *out)
{
    memset(out, 0, sizeof(*out));
    U32 total, cd_size, cd_off;
    if (!zip_find_eocd(data, &total, &cd_size, &cd_off)) return 0;
    if ((U64)cd_off + cd_size > data.size) return 0;
    out->data = data;
    return zip_parse_central(a, str8(data.str + cd_off, cd_size), total, out);
}


// Returns the entry's bytes, or ok=0 on corrupt data / unsupported method.
static String8 zip_read(Arena *a, ZipArchive *z, ZipEntry *e, B32 *ok)
{
    *ok = 0;
    String8 d = z->data;
    if ((U64)e->local_offset + 30 > d.size || rd32(d.str + e->local_offset) != 0x04034b50) return str8(0, 0);
    U64 start = (U64)e->local_offset + 30 + rd16(d.str + e->local_offset + 26) + rd16(d.str + e->local_offset + 28);
    if (start + e->comp_size > d.size) return str8(0, 0);
    String8 comp = str8(d.str + start, e->comp_size);
    U8 *out = push_array_nz(a, U8, (U64)e->size + 1);
    if (e->method == 0) {
        if (e->comp_size != e->size) return str8(0, 0);
        memcpy(out, comp.str, e->size);
    } else if (e->method == 8) {
        if (!inflate_raw(comp, out, e->size)) return str8(0, 0);
    } else {
        return str8(0, 0);
    }
    String8 result = str8(out, e->size);
    *ok = crc32(result) == e->crc;
    return result;
}

// Opens a zip on disk by reading only its tail and central directory (a few KB even for
// a huge file). Entries can then be read one by one with zip_read_file_entry.
static B32 zip_open_file(Arena *a, String8 path, U64 file_size, ZipArchive *out)
{
    memset(out, 0, sizeof(*out));
    U64 tail_size = Min(file_size, 65535ull + 22);
    String8 tail = os_read_file_range(a, path, file_size - tail_size, tail_size);
    U32 total, cd_size, cd_off;
    if (!zip_find_eocd(tail, &total, &cd_size, &cd_off) || (U64)cd_off + cd_size > file_size || cd_size > MB(64)) return 0;
    String8 cd = os_read_file_range(a, path, cd_off, cd_size);
    if (cd.size != cd_size) return 0;
    return zip_parse_central(a, cd, total, out);
}

static String8 zip_read_file_entry(Arena *a, String8 path, ZipEntry *e, B32 *ok)
{
    *ok = 0;
    if (e->size > MB(16) || e->comp_size > MB(16)) return str8(0, 0); // only used for small files (.toc)
    String8 hdr = os_read_file_range(a, path, e->local_offset, 30);
    if (hdr.size != 30 || rd32(hdr.str) != 0x04034b50) return str8(0, 0);
    U64 start = (U64)e->local_offset + 30 + rd16(hdr.str + 26) + rd16(hdr.str + 28);
    String8 comp = os_read_file_range(a, path, start, e->comp_size);
    if (comp.size != e->comp_size) return str8(0, 0);
    U8 *out = push_array_nz(a, U8, (U64)e->size + 1);
    if (e->method == 0 && e->comp_size == e->size) memcpy(out, comp.str, e->size);
    else if (e->method != 8 || !inflate_raw(comp, out, e->size)) return str8(0, 0);
    String8 result = str8(out, e->size);
    *ok = crc32(result) == e->crc;
    return result;
}

// Rejects absolute paths, drive letters and ".." so an archive can't write outside its target.
static B32 zip_name_is_safe(String8 name)
{
    if (!name.size || name.str[0] == '/' || name.str[0] == '\\') return 0;
    for (U64 i = 0; i < name.size; i += 1) if (name.str[i] == ':') return 0;
    String8 rest = name;
    while (rest.size) {
        U64 i = 0;
        while (i < rest.size && rest.str[i] != '/' && rest.str[i] != '\\') i += 1;
        String8 part = str8_prefix(rest, i);
        if (str8_match(part, str8_lit(".."), 0)) return 0;
        rest = str8_skip(rest, i + 1);
    }
    return 1;
}
