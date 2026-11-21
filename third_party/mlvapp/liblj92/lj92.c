/*
lj92.c
(c) Andrew Baldwin 2014

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is furnished to do
so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lj92.h"

/* MLV Drive: MSVC has no __builtin_clz */
#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
static __inline int lj92_clz(unsigned int x) {
    unsigned long index;
    return _BitScanReverse(&index, x) ? 31 - (int)index : 32;
}
#define __builtin_clz lj92_clz
#endif

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;

//#define SLOW_HUFF
//#define DEBUG

typedef struct _ljp {
    u8* data;
    u8* dataend;
    int datalen;
    int scanstart;
    int ix;
    int x; // Width
    int y; // Height
    int bits; // Bit depth
    int components;  // Components(Nf)
    int writelen; // Write rows this long
    int skiplen; // Skip this many values after each row
    u16* linearize; // Linearization table
    int linlen;
    //int sssshist[16];

    // Huffman table - only one supported, and probably needed
#ifdef SLOW_HUFF
    int* maxcode;
    int* mincode;
    int* valptr;
    u8* huffval;
    int* huffsize;
    int* huffcode;
#else
    u16* hufflut;
    int huffbits;
#endif
    // Parse state
    int cnt;
    u32 b;
    u16* image;
    u16* rowcache;
    u16* outrow[2];
} ljp;

static int find(ljp* self) {
    int ix = self->ix;
    u8* data = self->data;
    while (data[ix] != 0xFF && ix<(self->datalen-1)) {
        ix += 1;
    }
    ix += 2;
    if (ix>=self->datalen) return -1;
    self->ix = ix;
    return data[ix-1];
}

#define BEH(ptr) ((((int)(*&ptr))<<8)|(*(&ptr+1)))

static int parseHuff(ljp* self) {
    int ret = LJ92_ERROR_CORRUPT;
    u8* huffhead = &self->data[self->ix]; // xstruct.unpack('>HB16B',self.data[self.ix:self.ix+19])
    u8* bits = &huffhead[2];
    bits[0] = 0; // Because table starts from 1
    int hufflen = BEH(huffhead[0]);
    if ((self->ix + hufflen) >= self->datalen) return ret;
#ifdef SLOW_HUFF
    u8* huffval = calloc(hufflen - 19,sizeof(u8));
    if (huffval == NULL) return LJ92_ERROR_NO_MEMORY;
    self->huffval = huffval;
    for (int hix=0;hix<(hufflen-19);hix++) {
        huffval[hix] = self->data[self->ix+19+hix];
#ifdef DEBUG
        printf("huffval[%d]=%d\n",hix,huffval[hix]);
#endif
    }
    self->ix += hufflen;
    // Generate huffman table
    int k = 0;
    int i = 1;
    int j = 1;
    int huffsize_needed = 1;
    // First calculate how long huffsize needs to be
    while (i<=16) {
        while (j<=bits[i]) {
            huffsize_needed++;
            k = k+1;
            j = j+1;
        }
        i = i+1;
        j = 1;
    }
    // Now allocate and do it
    int* huffsize = calloc(huffsize_needed,sizeof(int));
    if (huffsize == NULL) return LJ92_ERROR_NO_MEMORY;
    self->huffsize = huffsize;
    k = 0;
    i = 1;
    j = 1;
    // First calculate how long huffsize needs to be
    int hsix = 0;
    while (i<=16) {
        while (j<=bits[i]) {
            huffsize[hsix++] = i;
            k = k+1;
            j = j+1;
        }
        i = i+1;
        j = 1;
    }
    huffsize[hsix++] = 0;

    // Calculate the size of huffcode array
    int huffcode_needed = 0;
    k = 0;
    int code = 0;
    int si = huffsize[0];
    while (1) {
        while (huffsize[k] == si) {
            huffcode_needed++;
            code = code+1;
            k = k+1;
        }
        if (huffsize[k] == 0)
            break;
        while (huffsize[k] != si) {
            code = code << 1;
            si = si + 1;
        }
    }
    // Now fill it
    int* huffcode = calloc(huffcode_needed,sizeof(int));
    if (huffcode == NULL) return LJ92_ERROR_NO_MEMORY;
    self->huffcode = huffcode;
    int hcix = 0;
    k = 0;
    code = 0;
    si = huffsize[0];
    while (1) {
        while (huffsize[k] == si) {
            huffcode[hcix++] = code;
            code = code+1;
            k = k+1;
        }
        if (huffsize[k] == 0)
            break;
        while (huffsize[k] != si) {
            code = code << 1;
            si = si + 1;
        }
    }

    i = 0;
    j = 0;

    int* maxcode = calloc(17,sizeof(int));
    if (maxcode == NULL) return LJ92_ERROR_NO_MEMORY;
    self->maxcode = maxcode;
    int* mincode = calloc(17,sizeof(int));
    if (mincode == NULL) return LJ92_ERROR_NO_MEMORY;
    self->mincode = mincode;
    int* valptr = calloc(17,sizeof(int));
    if (valptr == NULL) return LJ92_ERROR_NO_MEMORY;
    self->valptr = valptr;

    while (1) {
        while (1) {
            i++;
            if (i>16)
                break;
            if (bits[i]!=0)
                break;
            maxcode[i] = -1;
        }
        if (i>16)
            break;
        valptr[i] = j;
        mincode[i] = huffcode[j];
        j = j+bits[i]-1;
        maxcode[i] = huffcode[j];
        j++;
    }
    free(huffsize);
    self->huffsize = NULL;
    free(huffcode);
    self->huffcode = NULL;
    ret = LJ92_ERROR_NONE;
#else
    /* Calculate huffman direct lut */
    // How many bits in the table - find highest entry
    u8* huffvals = &self->data[self->ix+19];
    int maxbits = 16;
    while (maxbits>0) {
        if (bits[maxbits]) break;
        maxbits--;
    }
    self->huffbits = maxbits;
    /* Now fill the lut */
    u16* hufflut = malloc((1<<maxbits) * sizeof(u16));
    if (hufflut == NULL) return LJ92_ERROR_NO_MEMORY;
    self->hufflut = hufflut;
    int i = 0;
    int hv = 0;
    int rv = 0;
    int vl = 0; // i
    int hcode;
    int bitsused = 1;
#ifdef DEBUG
    printf("%04x:%x:%d:%x\n",i,huffvals[hv],bitsused,1<<(maxbits-bitsused));
#endif
    while (i<1<<maxbits) {
        if (bitsused>maxbits) {
            break; // Done. Should never get here!
        }
        if (vl >= bits[bitsused]) {
            bitsused++;
            vl = 0;
            continue;
        }
        if (rv == 1 << (maxbits-bitsused)) {
            rv = 0;
            vl++;
            hv++;
#ifdef DEBUG
            printf("%04x:%x:%d:%x\n",i,huffvals[hv],bitsused,1<<(maxbits-bitsused));
#endif
            continue;
        }
        hcode = huffvals[hv];
        hufflut[i] = hcode<<8 | bitsused;
        //printf("%d %d %d\n",i,bitsused,hcode);
        i++;
        rv++;
    }
    ret = LJ92_ERROR_NONE;
#endif
    return ret;
}

static int parseSof3(ljp* self) {
    if (self->ix+6 >= self->datalen) return LJ92_ERROR_CORRUPT;
    self->y = BEH(self->data[self->ix+3]);
    self->x = BEH(self->data[self->ix+5]);
    self->bits = self->data[self->ix+2];
    self->components = self->data[self->ix + 7];
    self->ix += BEH(self->data[self->ix]);
    return LJ92_ERROR_NONE;
}

static int parseBlock(ljp* self) {
    self->ix += BEH(self->data[self->ix]);
    if (self->ix >= self->datalen) return LJ92_ERROR_CORRUPT;
    return LJ92_ERROR_NONE;
}

#ifdef SLOW_HUFF
static int nextbit(ljp* self) {
    u32 b = self->b;
    if (self->cnt == 0) {
        u8* data = &self->data[self->ix];
        u32 next = *data++;
        b = next;
        if (next == 0xff) {
            data++;
            self->ix++;
        }
        self->ix++;
        self->cnt = 8;
    }
    int bit = b >> 7;
    self->cnt--;
    self->b = (b << 1)&0xFF;
    return bit;
}

static int decode(ljp* self) {
    int i = 1;
    int code = nextbit(self);
    while (code > self->maxcode[i]) {
        i++;
        code = (code << 1) + nextbit(self);
    }
    int j = self->valptr[i];
    j = j + code - self->mincode[i];
    int value = self->huffval[j];
    return value;
}

static int receive(ljp* self,int ssss) {
    if (ssss == 16) {
        return 1 << 15;
    }
    int i = 0;
    int v = 0;
    while (i != ssss) {
        i++;
        v = (v<<1) + nextbit(self);
    }
    return v;
}

static int extend(ljp* self,int v,int t) {
    int vt = 1<<(t-1);
    if (v < vt) {
        vt = (-1 << t) + 1;
        v = v + vt;
    }
    return v;
}
#endif

inline static int nextdiff(ljp* self) {
#ifdef SLOW_HUFF
    int t = decode(self);
    int diff = receive(self,t);
    diff = extend(self,diff,t);
#else
    u32 b = self->b;
    int cnt = self->cnt;
    int huffbits = self->huffbits;
    int ix = self->ix;
    int next;
    while (cnt < huffbits) {
        next = *(u16*)&self->data[ix];
        int one = next&0xFF;
        int two = next>>8;
        b = (b<<16)|(one<<8)|two;
        cnt += 16;
        ix += 2;
        if (one==0xFF) {
            //printf("%x %x %x %x %d\n",one,two,b,b>>8,cnt);
            b >>= 8;
            cnt -= 8;
        } else if (two==0xFF) ix++;
    }
    int index = b >> (cnt - huffbits);
    u16 ssssused = self->hufflut[index];
    int usedbits = ssssused&0xFF;
    int t = ssssused>>8;
    //self->sssshist[t]++;
    cnt -= usedbits;
    int keepbitsmask = (1 << cnt)-1;
    b &= keepbitsmask;
    int diff;
    if (t == 16) {
        diff = 1 << 15;
    } else {
        while (cnt < t) {
            next = *(u16*)&self->data[ix];
            int one = next&0xFF;
            int two = next>>8;
            b = (b<<16)|(one<<8)|two;
            cnt += 16;
            ix += 2;
            if (one==0xFF) {
                b >>= 8;
                cnt -= 8;
            } else if (two==0xFF) ix++;
        }
        cnt -= t;
        diff = b >> cnt;
        int vt = 1<<(t-1);
        if (diff < vt) {
            vt = (-1 << t) + 1;
            diff += vt;
        }
    }
    keepbitsmask = (1 << cnt)-1;
    self->b = b & keepbitsmask;
    self->cnt = cnt;
    self->ix = ix;
    //printf("%d %d\n",t,diff);
#ifdef DEBUG
#endif
#endif
    return diff;
}

static int parsePred6(ljp* self) {
    int ret = LJ92_ERROR_CORRUPT;
    self->ix = self->scanstart;
    //int compcount = self->data[self->ix+2];
    self->ix += BEH(self->data[self->ix]);
    self->cnt = 0;
    self->b = 0;
    int write = self->writelen;
    // Now need to decode huffman coded values
    int c = 0;
    int pixels = self->y * self->x;
    u16* out = self->image;
    u16* temprow;
    u16* thisrow = self->outrow[0];
    u16* lastrow = self->outrow[1];

    // First pixel predicted from base value
    int diff;
    int Px;
    int col = 0;
    //int row = 0;
    int left = 0;
    int linear;

    // First pixel
    diff = nextdiff(self);
    Px = 1 << (self->bits-1);
    left = Px + diff;
    left = (u16) (left%65536);
    if (self->linearize)
        linear = self->linearize[left];
    else
        linear = left;
    thisrow[col++] = left;
    out[c++] = linear;
    if (self->ix >= self->datalen) return ret;
    --write;
    int rowcount = self->x-1;
    while (rowcount--) {
        diff = nextdiff(self);
        Px = left;
        left = Px + diff;
        left = (u16) (left%65536);
        if (self->linearize)
            linear = self->linearize[left];
        else
            linear = left;
        thisrow[col++] = left;
        out[c++] = linear;
        //printf("%d %d %d %d %x\n",col-1,diff,left,thisrow[col-1],&thisrow[col-1]);
        if (self->ix >= self->datalen) return ret;
        if (--write==0) {
            out += self->skiplen;
            write = self->writelen;
        }
    }
    temprow = lastrow;
    lastrow = thisrow;
    thisrow = temprow;
    //row++;
    //printf("%x %x\n",thisrow,lastrow);
    while (c<pixels) {
        col = 0;
        diff = nextdiff(self);
        Px = lastrow[col]; // Use value above for first pixel in row
        left = Px + diff;
        left = (u16) (left%65536);
        if (self->linearize) {
            if (left>self->linlen) return LJ92_ERROR_CORRUPT;
            linear = self->linearize[left];
        } else
            linear = left;
        thisrow[col++] = left;
        //printf("%d %d %d %d\n",col,diff,left,lastrow[col]);
        out[c++] = linear;
        if (self->ix >= self->datalen) break;
        rowcount = self->x-1;
        if (--write==0) {
            out += self->skiplen;
            write = self->writelen;
        }
        while (rowcount--) {
            diff = nextdiff(self);
            Px = lastrow[col] + ((left - lastrow[col-1])>>1);
            left = Px + diff;
            left = (u16) (left%65536);
            //printf("%d %d %d %d %d %x\n",col,diff,left,lastrow[col],lastrow[col-1],&lastrow[col]);
            if (self->linearize) {
                if (left>self->linlen) return LJ92_ERROR_CORRUPT;
                linear = self->linearize[left];
            } else
                linear = left;
            thisrow[col++] = left;
            out[c++] = linear;
            if (--write==0) {
                out += self->skiplen;
                write = self->writelen;
            }
        }
        temprow = lastrow;
        lastrow = thisrow;
        thisrow = temprow;
        if (self->ix >= self->datalen) break;
    }
    if (c >= pixels) ret = LJ92_ERROR_NONE;
    return ret;
}

int lj92_fast = 1;  /* MLV Drive: 0 decodes and encodes with the original code (for comparing) */

/* MLV Drive: a faster decoder for the common streams (predictor 1 or 6, any number of
 * interleaved components, the image written contiguously, no linearisation table): camera MLV
 * frames are 2 components with predictor 1, MLV-App's own 1 component with
 * predictor 6. The output is the same as the generic decoder's, value for value.
 *
 * The bit reader refills 8 bytes at a time straight from the data, without a branch on the data,
 * whenever none of them is 0xFF (the start of a stuffed byte or a marker): then 56 to 63 bits
 * wait. Near a 0xFF (a few % of refills) it goes byte by byte. One lookup in a 12-bit table gives
 * the difference itself for most samples; where only a code's extra bits don't fit, they are read
 * straight from the buffer; only codes longer than 12 bits take the full table. As the original
 * reader, data past the end or a marker reads as zeros (corrupt data decodes to wrong values,
 * never out of bounds). (Copying the data without its stuffing first, to refill with no check at
 * all, was as fast on one thread but slower on all of them: the copy's memory traffic.) */

#if defined(_MSC_VER)
#define lj92_bswap64 _byteswap_uint64
#else
#define lj92_bswap64 __builtin_bswap64
#endif

typedef struct {
    const u8* p;     /* next byte to load */
    const u8* end;   /* end of the data */
    uint64_t buf;    /* the bits not used yet, from the top */
    int bits;        /* how many */
} lj92_bits;

/* Byte by byte, until at least 32 bits wait: stuffed 0xFF, a marker, the end. */
static void lj92_refill_bytes(lj92_bits* r) {
    while (r->bits < 32) {
        u32 byte = 0;
        if (r->p < r->end) {
            byte = *r->p;
            if (byte == 0xFF) {
                if (r->p + 1 < r->end && r->p[1] == 0x00) r->p += 2;  /* stuffed 0xFF */
                else byte = 0;                                       /* a marker: the end */
            } else {
                r->p++;
            }
        }
        r->buf |= (uint64_t)byte << (56 - r->bits);
        r->bits += 8;
    }
}

/* Fills the buffer to 56..63 bits when the next 8 bytes hold no 0xFF (the bytes below the bits
 * waiting may be loaded twice: the same bits, OR-ed again); else byte by byte. */
static inline void lj92_refill(lj92_bits* r) {
    if (r->p + 8 <= r->end) {
        uint64_t w;
        memcpy(&w, r->p, 8);
        const uint64_t t = ~w;
        if (((t - 0x0101010101010101ull) & ~t & 0x8080808080808080ull) == 0) {  /* no 0xFF among them */
            r->buf |= lj92_bswap64(w) >> r->bits;
            r->p += (63 - r->bits) >> 3;
            r->bits |= 56;
            return;
        }
    }
    lj92_refill_bytes(r);
}

/* The combined table: indexed by the next LJ92_FASTBITS bits; where the code and its extra bits
 * both fit, the difference itself (entry = diff << 8 | bits used); else 0 in the low byte, the
 * code's length and its count of extra bits above (entry = t << 16 | len << 8), the length 0 or
 * over LJ92_FASTBITS for a code longer than the table. */
#ifndef LJ92_FASTBITS
#define LJ92_FASTBITS 12  /* measured fastest (12: 3.70 ns a pixel, 13: 3.73, 14: 4.03) */
#endif

static void lj92_fill_fast(int32_t* fast, const u16* lut, int lutbits) {
    const int N = LJ92_FASTBITS;
    for (int idx = 0; idx < (1 << N); idx++) {
        /* the code: its first bits are idx's (a shorter code is decided by fewer of them) */
        int k = lutbits <= N ? idx >> (N - lutbits) : idx << (lutbits - N);
        u16 e = lut[k];
        int len = e & 0xFF, t = e >> 8;
        int32_t entry = (int32_t)(t << 16 | len << 8);  /* the slow form */
        if (len > 0 && len <= N) {
            if (t == 16) {
                entry = (int32_t)(32768u << 8) | len;
            } else if (t == 0) {
                entry = len;  /* diff 0 */
            } else if (len + t <= N) {
                int v = (idx >> (N - len - t)) & ((1 << t) - 1);
                int diff = v < (1 << (t - 1)) ? v - (1 << t) + 1 : v;
                entry = (int32_t)((uint32_t)diff << 8) | (len + t);
            }
        }
        fast[idx] = entry;
    }
}

/* A sample the 12-bit table doesn't finish: its code (from the entry, or for a long code from
 * the full table), then its extra bits. 32 bits are waiting: enough for a 16-bit code and 16
 * extra bits. */
static int lj92_diff_slow(lj92_bits* r, int32_t e, const u16* lut, int lutbits) {
    int len = (e >> 8) & 0xFF, t = (e >> 16) & 0xFF;
    if (len == 0 || len > LJ92_FASTBITS) {
        u16 f = lut[r->buf >> (64 - lutbits)];
        len = f & 0xFF;
        t = f >> 8;
    }
    r->buf <<= len;
    r->bits -= len;
    if (t == 0) return 0;
    if (t == 16) return 32768;  /* no extra bits */
    int v = (int)(r->buf >> (64 - t));
    r->buf <<= t;
    r->bits -= t;
    return v < (1 << (t - 1)) ? v - (1 << t) + 1 : v;
}

/* The next difference. */
static inline int lj92_diff_fast(lj92_bits* r, const int32_t* fast, const u16* lut, int lutbits) {
    if (r->bits < 32) lj92_refill(r);
    int32_t e = fast[r->buf >> (64 - LJ92_FASTBITS)];
    int used = e & 0xFF;
    if (used) {
        r->buf <<= used;
        r->bits -= used;
        return e >> 8;  /* arithmetic shift: the signed difference */
    }
    return lj92_diff_slow(r, e, lut, lutbits);
}

static int parseFast(ljp* self, int pred) {
    self->ix = self->scanstart;
    self->ix += BEH(self->data[self->ix]);
    lj92_bits r = { self->data + self->ix, self->data + self->datalen, 0, 0 };
    const u16* lut = self->hufflut;
    const int lutbits = self->huffbits;
    int32_t* fast = (int32_t*)malloc(sizeof(int32_t) << LJ92_FASTBITS);
    if (!fast) return LJ92_ERROR_NO_MEMORY;
    lj92_fill_fast(fast, lut, lutbits);
#define lj92_diff(r, lut, lutbits) lj92_diff_fast(r, fast, lut, lutbits)
    const int C = self->components;
    const int W = self->x * C;  /* samples in a row */
    const int H = self->y;
    u16* out = self->image;
    const int base = 1 << (self->bits - 1);
    int ret = LJ92_ERROR_NONE;
    if (W <= C || lutbits <= 0) ret = LJ92_ERROR_CORRUPT;

    for (int y = 0; y < H && ret == LJ92_ERROR_NONE; y++) {
        u16* row = out + (size_t)y * W;
        const u16* up = row - W;
        /* The first sample of each component: the start value, then the one above. */
        for (int c = 0; c < C; c++) {
            int px = y == 0 ? base : up[c];
            row[c] = (u16)(px + lj92_diff(&r, lut, lutbits));
        }
        if (y == 0 || pred == 1) {
            /* Predictor 1 (and every predictor's first row): the same component to the left. */
            for (int i = C; i < W; i++) row[i] = (u16)(row[i - C] + lj92_diff(&r, lut, lutbits));
        } else {
            /* Predictor 6: above + (left - above left) / 2. */
            for (int i = C; i < W; i++) {
                int px = up[i] + ((row[i - C] - up[i - C]) >> 1);
                row[i] = (u16)(px + lj92_diff(&r, lut, lutbits));
            }
        }
    }
    self->ix = (int)(r.p - self->data);
    free(fast);
    return ret;
}
#undef lj92_diff

static int parseScan(ljp* self) {
    int ret = LJ92_ERROR_CORRUPT;
    //memset(self->sssshist,0,sizeof(self->sssshist));
    self->ix = self->scanstart;
    int compcount = self->data[self->ix+2];
    int pred = self->data[self->ix+3+2*compcount];
    if (pred<0 || pred>7) return ret;
#ifndef SLOW_HUFF
    /* MLV Drive: the fast decoder, where the image is written whole and as it is */
    /* (predictor 6 with several components takes "left" from any component in the generic code
       below: only its single-component form, the one written in practice, is done here) */
    if (lj92_fast && (pred == 1 || (pred == 6 && self->components == 1)) && !self->linearize && self->skiplen == 0 &&
        self->writelen >= self->x * self->components * self->y && self->components >= 1 && self->components <= 4)
        return parseFast(self, pred);
#endif
    if (pred==6) return parsePred6(self); // Fast path
    self->ix += BEH(self->data[self->ix]);
    self->cnt = 0;
    self->b = 0;
    u16* out = self->image;
    u16* thisrow = self->outrow[0];
    u16* lastrow = self->outrow[1];

    // First pixel predicted from base value
    int diff;
    int Px = 0;
    int left = 0;
    for (int row = 0; row < self->y; row++) {
        for (int col = 0; col < self->x; col++) {
            int colx = col * self->components;
            for (int c = 0; c < self->components; c++) {
                if ((col==0)&&(row==0)) {
                    Px = 1 << (self->bits-1);
                } else if (row==0) {
                    // Px = left;
                    Px = thisrow[(col - 1) * self->components + c];
                } else if (col==0) {
                    Px = lastrow[c];  // Use value above for first pixel in row
                } else {
                    int prev_colx = (col - 1) * self->components;
   
                    switch (pred) {
                        case 0:
                          Px = 0;
                          break;  // No prediction... should not be used
                        case 1:
                          Px = thisrow[prev_colx + c];
                          break;
                        case 2:
                          Px = lastrow[colx + c];
                          break;
                        case 3:
                          Px = lastrow[prev_colx + c];
                          break;
                        case 4:
                          Px = left + lastrow[colx + c] - lastrow[prev_colx + c];
                          break;
                        case 5:
                          Px = left + ((lastrow[colx + c] - lastrow[prev_colx + c]) >> 1);
                          break;
                        case 6:
                          Px = lastrow[colx + c] + ((left - lastrow[prev_colx + c]) >> 1);
                          break;
                        case 7:
                          Px = (left + lastrow[colx + c]) >> 1;
                          break;
                    }
                }
                
                diff = nextdiff(self);
                left = Px + diff;
                left = (u16) (left%65536);
                //printf("%d %d %d\n",c,diff,left);
                int linear;
                if (self->linearize) {
                    if (left>self->linlen) return LJ92_ERROR_CORRUPT;
                    linear = self->linearize[left];
                } else
                    linear = left;

                thisrow[colx + c] = left;
                out[colx + c] = linear; // HACK
            } // c
        } // col

        u16* temprow = lastrow;
        lastrow = thisrow;
        thisrow = temprow;

        out += self->x * self->components + self->skiplen;
    } // row

    ret = LJ92_ERROR_NONE;
    return ret;
}

static int parseImage(ljp* self) {
    int ret = LJ92_ERROR_NONE;
    while (1) {
        int nextMarker = find(self);
        if (nextMarker == 0xc4)
            ret = parseHuff(self);
        else if (nextMarker == 0xc3)
            ret = parseSof3(self);
        else if (nextMarker == 0xfe)// Comment
            ret = parseBlock(self);
        else if (nextMarker == 0xd9) // End of image
            break;
        else if (nextMarker == 0xda) {
            self->scanstart = self->ix;
            ret = LJ92_ERROR_NONE;
            break;
        } else if (nextMarker == -1) {
            ret = LJ92_ERROR_CORRUPT;
            break;
        } else
            ret = parseBlock(self);
        if (ret != LJ92_ERROR_NONE) break;
    }
    return ret;
}

static int findSoI(ljp* self) {
    int ret = LJ92_ERROR_CORRUPT;
    if (find(self)==0xd8)
        ret = parseImage(self);
    return ret;
}

static void free_memory(ljp* self) {
#ifdef SLOW_HUFF
    free(self->maxcode);
    self->maxcode = NULL;
    free(self->mincode);
    self->mincode = NULL;
    free(self->valptr);
    self->valptr = NULL;
    free(self->huffval);
    self->huffval = NULL;
    free(self->huffsize);
    self->huffsize = NULL;
    free(self->huffcode);
    self->huffcode = NULL;
#else
    free(self->hufflut);
    self->hufflut = NULL;
#endif
    free(self->rowcache);
    self->rowcache = NULL;
}

int lj92_open(lj92* lj,
              uint8_t* data, int datalen,
              int* width,int* height, int* bitdepth, int* components) {
    ljp* self = (ljp*)calloc(sizeof(ljp),1);
    if (self==NULL) return LJ92_ERROR_NO_MEMORY;

    self->data = (u8*)data;
    self->dataend = self->data + datalen;
    self->datalen = datalen;

    int ret = findSoI(self);

    if (ret == LJ92_ERROR_NONE) {
        u16* rowcache = (u16*)calloc(self->x * self->components * 2, sizeof(u16));
        if (rowcache == NULL) ret = LJ92_ERROR_NO_MEMORY;
        else {
            self->rowcache = rowcache;
            self->outrow[0] = rowcache;
            /* MLV Drive: a row is x * components values (was &rowcache[x]: with 2 components, as
               camera MLV frames have, the two rows overlapped) */
            self->outrow[1] = &rowcache[self->x * self->components];
        }
    }

    if (ret != LJ92_ERROR_NONE) { // Failed, clean up
        *lj = NULL;
        free_memory(self);
        free(self);
    } else {
        *width = self->x;
        *height = self->y;
        *bitdepth = self->bits;
        *components = self->components;
        *lj = self;
    }
    return ret;
}

int lj92_decode(lj92 lj,
                uint16_t* target,int writeLength, int skipLength,
                uint16_t* linearize,int linearizeLength) {
    int ret = LJ92_ERROR_NONE;
    ljp* self = lj;
    if (self == NULL) return LJ92_ERROR_BAD_HANDLE;
    self->image = target;
    self->writelen = writeLength;
    self->skiplen = skipLength;
    self->linearize = linearize;
    self->linlen = linearizeLength;
    ret = parseScan(self);
    return ret;
}

void lj92_close(lj92 lj) {
    ljp* self = lj;
    if (self != NULL)
        free_memory(self);
    free(self);
}

/* Encoder implementation */

typedef struct _lje {
    uint16_t* image;
    int width;
    int height;
    int bitdepth;
    int readLength;
    int skipLength;
    uint16_t* delinearize;
    int delinearizeLength;
    uint8_t* encoded;
    int encodedWritten;
    int encodedLength;
    int hist[18]; // SSSS frequency histogram
    int bits[18];
    int huffval[18];
    u16 huffenc[18];
    u16 huffbits[18];
    int huffsym[18];
} lje;

int frequencyScan(lje* self) {
    // Scan through the tile using the standard type 6 prediction
    // Need to cache the previous 2 row in target coordinates because of tiling
    uint16_t* pixel = self->image;
    int pixcount = self->width*self->height;
    int scan = self->readLength;
    uint16_t* rowcache = (uint16_t*)calloc(1,self->width*4);
    uint16_t* rows[2];
    rows[0] = rowcache;
    rows[1] = &rowcache[self->width];

    int col = 0;
    int row = 0;
    int Px = 0;
    int32_t diff = 0;
    //int maxval = (1 << self->bitdepth);
    while (pixcount--) {
        uint16_t p = *pixel;
        /* if (self->delinearize) {
            if (p>=self->delinearizeLength) {
                free(rowcache);
                return LJ92_ERROR_TOO_WIDE;
            }
            p = self->delinearize[p];
        }
        if (p>=maxval) {
            free(rowcache);
            return LJ92_ERROR_TOO_WIDE;
        } */
        rows[1][col] = p;

        if ((row == 0)&&(col == 0))
            Px = 1 << (self->bitdepth-1);
        else if (row == 0)
            Px = rows[1][col-1];
        else if (col == 0)
            Px = rows[0][col];
        else
            Px = rows[0][col] + ((rows[1][col-1] - rows[0][col-1])>>1);
        diff = rows[1][col] - Px;
        diff = diff%65536;
        diff = (int16_t)diff;
        int ssss = 32 - __builtin_clz(abs(diff));
        if (diff==0) ssss=0;
        self->hist[ssss]++;
        //printf("%d %d %d %d %d %d\n",col,row,p,Px,diff,ssss);
        pixel++;
        scan--;
        col++;
        if (scan==0) { pixel += self->skipLength; scan = self->readLength; }
        if (col==self->width) {
            uint16_t* tmprow = rows[1];
            rows[1] = rows[0];
            rows[0] = tmprow;
            col=0;
            row++;
        }
    }
#ifdef DEBUG
    int sort[17];
    for (int h=0;h<17;h++) {
        sort[h] = h;
        printf("%d:%d\n",h,self->hist[h]);
    }
#endif
    free(rowcache);
    return LJ92_ERROR_NONE;
}

void createEncodeTable(lje* self) {
    float freq[18];
    int codesize[18];
    int others[18];

    // Calculate frequencies
    float totalpixels = self->width * self->height;
    for (int i=0;i<17;i++) {
        freq[i] = (float)(self->hist[i])/totalpixels;
#ifdef DEBUG
        printf("%d:%f\n",i,freq[i]);
#endif
        codesize[i] = 0;
        others[i] = -1;
    }
    codesize[17] = 0;
    others[17] = -1;
    freq[17] = 1.0f;

    float v1f,v2f;
    int v1,v2;

    while (1) {
        v1f=3.0f;
        v1=-1;
        for (int i=0;i<18;i++) {
            if ((freq[i]<=v1f) && (freq[i]>0.0f)) {
                v1f = freq[i];
                v1 = i;
            }
        }
#ifdef DEBUG
        printf("v1:%d,%f\n",v1,v1f);
#endif
        v2f=3.0f;
        v2=-1;
        for (int i=0;i<18;i++) {
            if (i==v1) continue;
            if ((freq[i]<v2f) && (freq[i]>0.0f)) {
                v2f = freq[i];
                v2 = i;
            }
        }
        if (v2==-1) break; // Done

        freq[v1] += freq[v2];
        freq[v2] = 0.0f;

        while (1) {
            codesize[v1]++;
            if (others[v1]==-1) break;
            v1 = others[v1];
        }
        others[v1] = v2;
        while (1) {
            codesize[v2]++;
            if (others[v2]==-1) break;
            v2 = others[v2];
        }
    }
    int* bits = self->bits;
    memset(bits,0,sizeof(self->bits));
    for (int i=0;i<18;i++) {
        if (codesize[i]!=0) {
            bits[codesize[i]]++;
        }
    }
#ifdef DEBUG
    for (int i=0;i<18;i++) {
        printf("bits:%d,%d,%d\n",i,bits[i],codesize[i]);
    }
#endif
    //adjust bits, this step is a must to remove a code with all ones
    //and fix bug of overriding SSSS-0 category with the code with all ones.
    int I = 17;
    while(1) {
        if(bits[I] > 0) {
            int J = I - 1;
            do {
                J = J - 1;
            } while(bits[J] <= 0);
            bits[I] = bits[I] - 2;
            bits[I - 1] = bits[I - 1] + 1; 
            bits[J + 1] = bits[J + 1] + 2;
            bits[J] = bits [J] - 1;
        } else {
            I = I - 1;
            if(I != 16) {
                continue;
            }
            while(bits[I] == 0) {
                I = I - 1;
            }
            bits[I] = bits[I] - 1;
            break;
        }  
    }
#ifdef DEBUG
for (int i=0;i<18;i++) {
    printf("Adjusted bits:%d,%d,%d\n",i,bits[i],codesize[i]);
}
#endif
    int* huffval = self->huffval;
    int i=1;
    int k=0;
    int j;
    memset(huffval,0,sizeof(self->huffval));
    while (i<=32) {
        j=0;
        while (j<17) {
            if (codesize[j]==i) {
                huffval[k++] = j;
            }
            j++;
        }
        i++;
    }
#ifdef DEBUG
    for (i=0;i<18;i++) {
        printf("i=%d,huffval[i]=%x\n",i,huffval[i]);
    }
#endif
    int maxbits = 16;
    while (maxbits>0) {
        if (bits[maxbits]) break;
        maxbits--;
    }
    u16* huffenc = self->huffenc;
    u16* huffbits = self->huffbits;
    int* huffsym = self->huffsym;
    memset(huffenc,0,sizeof(self->huffenc));
    memset(huffbits,0,sizeof(self->huffbits));
    memset(self->huffsym,0,sizeof(self->huffsym));
    i = 0;
    //int hv = 0;
    int rv = 0;
    int vl = 0; // i
    //int hcode;
    int bitsused = 1;
    int sym = 0;
    //printf("%04x:%x:%d:%x\n",i,huffvals[hv],bitsused,1<<(maxbits-bitsused));
    while (i<1<<maxbits) {
        if (bitsused>maxbits) {
            break; // Done. Should never get here!
        }
        if (vl >= bits[bitsused]) {
            bitsused++;
            vl = 0;
            continue;
        }
        if (rv == 1 << (maxbits-bitsused)) {
            rv = 0;
            vl++;
            //hv++;
            //printf("%04x:%x:%d:%x\n",i,huffvals[hv],bitsused,1<<(maxbits-bitsused));
            continue;
        }
        huffbits[sym] = bitsused;
        huffenc[sym++] = i>>(maxbits-bitsused);
        //printf("%d %d %d\n",i,bitsused,hcode);
        i+= (1<<(maxbits-bitsused));
        rv = 1<<(maxbits-bitsused);
    }
    for (i=0;i<18;i++) {
        if (huffbits[i]>0) {
            huffsym[huffval[i]] = i;
        }
#ifdef DEBUG
        printf("huffval[%d]=%d,huffenc[%d]=%x,bits=%d\n",i,huffval[i],i,huffenc[i],huffbits[i]);
#endif
        if (huffbits[i]>0) {
            huffsym[huffval[i]] = i;
        }
    }
#ifdef DEBUG
    for (i=0;i<18;i++) {
        printf("huffsym[%d]=%d\n",i,huffsym[i]);
    }
#endif
}

void writeHeader(lje* self) {
    int w = self->encodedWritten;
    uint8_t* e = self->encoded;
    e[w++] = 0xff; e[w++] = 0xd8; //SOI
    e[w++] = 0xff; e[w++] = 0xc4; //HUFF
    // Write HUFF
        int count = 0;
        for (int i=0;i<17;i++) {
            count += self->bits[i];
        }
        e[w++] = 0x0; e[w++] = 17+2+count; //Lf, frame header length
        e[w++] = 0; // Table ID
        for (int i=1;i<17;i++) {
            e[w++] = self->bits[i];
        }
        for (int i=0;i<count;i++) {
            e[w++] = self->huffval[i];
        }
    e[w++] = 0xff; e[w++] = 0xc3; //SOF3
        // Write SOF
        e[w++] = 0x0; e[w++] = 11; //Lf, frame header length
        e[w++] = self->bitdepth;
        e[w++] = self->height>>8; e[w++] = self->height&0xFF;
        e[w++] = self->width>>8; e[w++] = self->width&0xFF;
        e[w++] = 1; // Components
        e[w++] = 0; // Component ID
        e[w++] = 0x11; // Component X/Y
        e[w++] = 0; // Unused (Quantisation)
    e[w++] = 0xff; e[w++] = 0xda; //SCAN
    // Write SCAN
        e[w++] = 0x0; e[w++] = 8; //Ls, scan header length
        e[w++] = 1; // Components
        e[w++] = 0; //
        e[w++] = 0; //
        e[w++] = 6; // Predictor
        e[w++] = 0; //
        e[w++] = 0; //
    self->encodedWritten = w;
}

void writePost(lje* self) {
    int w = self->encodedWritten;
    uint8_t* e = self->encoded;
    e[w++] = 0xff; e[w++] = 0xd9; //EOI
    self->encodedWritten = w;
}

int writeBody(lje* self) {
    // Scan through the tile using the standard type 6 prediction
    // Need to cache the previous 2 row in target coordinates because of tiling
    uint16_t* pixel = self->image;
    int pixcount = self->width*self->height;
    int scan = self->readLength;
    uint16_t* rowcache = (uint16_t*)calloc(1,self->width*4);
    uint16_t* rows[2];
    rows[0] = rowcache;
    rows[1] = &rowcache[self->width];

    int col = 0;
    int row = 0;
    int Px = 0;
    int32_t diff = 0;
    //int bitcount = 0;
    uint8_t* out = self->encoded;
    int w = self->encodedWritten;
    uint8_t next = 0;
    uint8_t nextbits = 8;
    while (pixcount--) {
        uint16_t p = *pixel;
        if (self->delinearize) p = self->delinearize[p];
        rows[1][col] = p;

        if ((row == 0)&&(col == 0))
            Px = 1 << (self->bitdepth-1);
        else if (row == 0)
            Px = rows[1][col-1];
        else if (col == 0)
            Px = rows[0][col];
        else
            Px = rows[0][col] + ((rows[1][col-1] - rows[0][col-1])>>1);
        diff = rows[1][col] - Px;
        diff = diff%65536;
        diff = (int16_t)diff;
        int ssss = 32 - __builtin_clz(abs(diff));
        if (diff==0) ssss=0;
        //printf("%d %d %d %d %d\n",col,row,Px,diff,ssss);

        // Write the huffman code for the ssss value
        int huffcode = self->huffsym[ssss];
        int huffenc = self->huffenc[huffcode];
        int huffbits = self->huffbits[huffcode];

        int vt = ssss>0?(1<<(ssss-1)):0;
        //printf("%d %d %d %d\n",rows[1][col],Px,diff,Px+diff);
#ifdef DEBUG
#endif
        if (diff < vt)
            diff += (1 << (ssss))-1;

        // Write the ssss
        while (huffbits>0) {
            int usebits = huffbits>nextbits?nextbits:huffbits;
            // Add top usebits from huffval to next usebits of nextbits
            int tophuff = huffenc >> (huffbits - usebits);
            next |= (tophuff << (nextbits-usebits));
            nextbits -= usebits;
            huffbits -= usebits;
            huffenc &= (1<<huffbits)-1;
            if (nextbits==0) {
                if(w >= self->encodedLength - 1)
                {
                    free(rowcache);
                    return LJ92_ERROR_ENCODER;
                }
                out[w++] = next;
                if (next==0xff) out[w++] = 0x0;
                next = 0;
                nextbits = 8;
            }
        }
        // Write the rest of the bits for the value
        if (ssss == 16) {
            // Diff values (always 32678) for SSSS=16 are encoded with 0 bits
            ssss = 0;
        }
        while (ssss>0) {
            int usebits = ssss>nextbits?nextbits:ssss;
            // Add top usebits from huffval to next usebits of nextbits
            int tophuff = diff >> (ssss - usebits);
            next |= (tophuff << (nextbits-usebits));
            nextbits -= usebits;
            ssss -= usebits;
            diff &= (1<<ssss)-1;
            if (nextbits==0) {
                if(w >= self->encodedLength - 1)
                {
                    free(rowcache);
                    return LJ92_ERROR_ENCODER;
                }
                out[w++] = next;
                if (next==0xff) out[w++] = 0x0;
                next = 0;
                nextbits = 8;
            }
        }

        //printf("%d %d\n",diff,ssss);
        pixel++;
        scan--;
        col++;
        if (scan==0) { pixel += self->skipLength; scan = self->readLength; }
        if (col==self->width) {
            uint16_t* tmprow = rows[1];
            rows[1] = rows[0];
            rows[0] = tmprow;
            col=0;
            row++;
        }
    }
    // Flush the final bits
    if (nextbits<8) {
        out[w++] = next;
        if (next==0xff) out[w++] = 0x0;
    }
#ifdef DEBUG
    int sort[18];
    for (int h=0;h<18;h++) {
        sort[h] = h;
        printf("%d:%d\n",h,self->hist[h]);
    }
    printf("Total bytes: %d\n",bitcount>>3);
#endif
    free(rowcache);
    self->encodedWritten = w;
    return LJ92_ERROR_NONE;
}
/* MLV Drive: a faster encoder for an image read whole (no delinearisation, no skipping): the
 * same predictions, histogram, table and bits as frequencyScan + writeBody above, byte for byte.
 * A row's differences are computed once into a small buffer (the first row and column outside
 * the inner loop), and a 64-bit bit writer stores 4 bytes at once when none is 0xFF. */

/* The differences of one row (type 6 prediction; the first row from the left, the first column
 * from above), as the original computes them: (cur - Px) wrapped to 16 bits, signed. */
static void lj92_row_diffs(const u16* row, const u16* up, int W, int base, int16_t* d) {
    if (!up) {
        d[0] = (int16_t)(row[0] - base);
        for (int i = 1; i < W; i++) d[i] = (int16_t)(row[i] - row[i - 1]);
        return;
    }
    d[0] = (int16_t)(row[0] - up[0]);
    for (int i = 1; i < W; i++) {
        int px = up[i] + ((row[i - 1] - up[i - 1]) >> 1);
        d[i] = (int16_t)(row[i] - px);
    }
}

/* The size category: the bits in |diff| (0 for 0), with no branch: 2|diff|+1 is never 0 */
static inline int lj92_ssss(int diff) {
    unsigned a = (unsigned)(diff < 0 ? -diff : diff);
    return 31 - __builtin_clz(2 * a + 1);
}

static int fastEncode(lje* self) {
    const int W = self->width, H = self->height;
    const int base = 1 << (self->bitdepth - 1);
    int16_t* d = (int16_t*)malloc(sizeof(int16_t) * (size_t)W);
    if (!d) return LJ92_ERROR_NO_MEMORY;

    /* Pass 1: the size categories' histogram (4 of them in turn, added up: a run of the same
       category doesn't wait on one counter) */
    int hist[4][17];
    memset(hist, 0, sizeof hist);
    for (int y = 0; y < H; y++) {
        const u16* row = self->image + (size_t)y * W;
        lj92_row_diffs(row, y ? row - W : NULL, W, base, d);
        int i = 0;
        for (; i + 4 <= W; i += 4) {
            hist[0][lj92_ssss(d[i])]++;
            hist[1][lj92_ssss(d[i + 1])]++;
            hist[2][lj92_ssss(d[i + 2])]++;
            hist[3][lj92_ssss(d[i + 3])]++;
        }
        for (; i < W; i++) hist[0][lj92_ssss(d[i])]++;
    }
    for (int k = 0; k < 17; k++) self->hist[k] += hist[0][k] + hist[1][k] + hist[2][k] + hist[3][k];
    createEncodeTable(self);
    writeHeader(self);

    /* Pass 2: the codes, through a 64-bit accumulator (bits waiting at its bottom) */
    /* per category: the code, its length, the count of value bits after it (none for 0 and 16) */
    uint32_t code[17];
    int length[17], extra[17];
    for (int s = 0; s <= 16; s++) {
        int hc = self->huffsym[s];
        code[s] = self->huffenc[hc];
        length[s] = self->huffbits[hc];
        extra[s] = s < 16 ? s : 0;
    }
    uint8_t* out = self->encoded;
    int w = self->encodedWritten;
    const int limit = self->encodedLength - 16;  /* room for a flush with every byte stuffed */
    uint64_t acc = 0;
    int nbits = 0;
    int ret = LJ92_ERROR_NONE;
    for (int y = 0; y < H && ret == LJ92_ERROR_NONE; y++) {
        const u16* row = self->image + (size_t)y * W;
        lj92_row_diffs(row, y ? row - W : NULL, W, base, d);
        for (int i = 0; i < W; i++) {
            int diff = d[i];
            int s = lj92_ssss(diff);
            /* the code, then the value's low bits: a negative one as diff + 2^s - 1, which in
               s bits is diff - 1 */
            int e = extra[s];
            uint32_t value = (uint32_t)(diff - (diff < 0)) & ((1u << e) - 1);
            acc = (((acc << length[s]) | code[s]) << e) | value;
            nbits += length[s] + e;
            if (nbits >= 32) {
                if (w >= limit) { ret = LJ92_ERROR_ENCODER; break; }
                uint32_t word = (uint32_t)(acc >> (nbits - 32));
                uint32_t t = ~word;
                if (((t - 0x01010101u) & ~t & 0x80808080u) == 0) {  /* no 0xFF: 4 bytes at once */
                    out[w] = (uint8_t)(word >> 24); out[w + 1] = (uint8_t)(word >> 16);
                    out[w + 2] = (uint8_t)(word >> 8); out[w + 3] = (uint8_t)word;
                    w += 4;
                } else {
                    for (int k = 24; k >= 0; k -= 8) {
                        uint8_t b = (uint8_t)(word >> k);
                        out[w++] = b;
                        if (b == 0xFF) out[w++] = 0x00;
                    }
                }
                nbits -= 32;
            }
        }
    }
    /* The last bits, whole bytes, then a final byte padded with zeros */
    while (ret == LJ92_ERROR_NONE && nbits > 0) {
        if (w >= self->encodedLength - 1) { ret = LJ92_ERROR_ENCODER; break; }
        uint8_t b = nbits >= 8 ? (uint8_t)(acc >> (nbits - 8)) : (uint8_t)(acc << (8 - nbits));
        out[w++] = b;
        if (b == 0xFF) out[w++] = 0x00;
        nbits = nbits >= 8 ? nbits - 8 : 0;
    }
    free(d);
    self->encodedWritten = w;
    return ret;
}

/* Encoder
 * Read tile from an image and encode in one shot
 * Return the encoded data
 */
int lj92_encode(uint16_t* image, int width, int height, int bitdepth,
                int readLength, int skipLength,
                uint16_t* delinearize,int delinearizeLength,
                uint8_t** encoded, int* encodedLength) {
    int ret = LJ92_ERROR_NONE;

    lje* self = (lje*)calloc(sizeof(lje),1);
    if (self==NULL) return LJ92_ERROR_NO_MEMORY;
    self->image = image;
    self->width = width;
    self->height = height;
    self->bitdepth = bitdepth;
    self->readLength = readLength;
    self->skipLength = skipLength;
    self->delinearize = delinearize;
    self->delinearizeLength = delinearizeLength;
    self->encodedLength = width*height*3+200;
    self->encoded = malloc(self->encodedLength);
    if (self->encoded==NULL) { free(self); return LJ92_ERROR_NO_MEMORY; }
    if (lj92_fast && !delinearize && skipLength == 0 && width > 0 && height > 0) {
        /* MLV Drive: the image read whole: the fast encoder (histogram, table, header, body) */
        ret = fastEncode(self);
        if (ret != LJ92_ERROR_NONE) {
            free(self->encoded);
            free(self);
            return ret;
        }
        writePost(self);
        self->encoded = realloc(self->encoded,self->encodedWritten);
        *encoded = self->encoded;
        *encodedLength = self->encodedWritten;
        free(self);
        return LJ92_ERROR_NONE;
    }
    // Scan through data to gather frequencies of ssss prefixes
    ret = frequencyScan(self);
    if (ret != LJ92_ERROR_NONE) {
        free(self->encoded);
        free(self);
        return ret;
    }
    // Create encoded table based on frequencies
    createEncodeTable(self);
    // Write JPEG head and scan header
    writeHeader(self);
    // Scan through and do the compression
    ret = writeBody(self);
    if (ret != LJ92_ERROR_NONE) {
        free(self->encoded);
        free(self);
        return ret;
    }
    // Finish
    writePost(self);
#ifdef DEBUG
    printf("written:%d\n",self->encodedWritten);
#endif
    self->encoded = realloc(self->encoded,self->encodedWritten);
    self->encodedLength = self->encodedWritten;
    *encoded = self->encoded;
    *encodedLength = self->encodedLength;

    free(self);

    return ret;
}

