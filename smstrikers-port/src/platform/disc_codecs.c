// strikers-quest r41: the .rvz/.wia decompressors besides zstd, for src/platform/disc.c. Built in
// its own library, without the port prelude: the LZMA SDK's 7zTypes.h defines Byte, UInt32, True
// and the like, which have no business next to the game's types.

#include <stdlib.h>
#include <string.h>

#include "LzmaDec.h"
#include "Lzma2Dec.h"
#include "bzlib.h"

static void* sz_alloc(ISzAllocPtr p, size_t size)
{
    (void)p;
    return malloc(size);
}

static void sz_free(ISzAllocPtr p, void* address)
{
    (void)p;
    free(address);
}

static const ISzAlloc kAlloc = { sz_alloc, sz_free };

// One raw LZMA (lzma2 = 0; props = the 5-byte properties) or LZMA2 (lzma2 = 1; props[0] = the
// dictionary byte) stream, as Dolphin writes them, into `out`. Bytes produced, or -1.
long port_disc_unlzma(const unsigned char* props, size_t propsLen, int lzma2, const unsigned char* in,
                      size_t inLen, unsigned char* out, size_t outCap)
{
    SizeT destLen = outCap;
    SizeT srcLen = inLen;
    ELzmaStatus status;
    SRes res;
    if (lzma2)
    {
        if (propsLen < 1)
            return -1;
        res = Lzma2Decode(out, &destLen, in, &srcLen, props[0], LZMA_FINISH_ANY, &status, &kAlloc);
    }
    else
    {
        if (propsLen < LZMA_PROPS_SIZE)
            return -1;
        res = LzmaDecode(out, &destLen, in, &srcLen, props, LZMA_PROPS_SIZE, LZMA_FINISH_ANY, &status,
                         &kAlloc);
    }
    return res == SZ_OK ? (long)destLen : -1;
}

// One bzip2 stream into `out`: all of it, or as much as `out` holds (like LZMA_FINISH_ANY above;
// BZ2_bzBuffToBuffDecompress would call a stream that fills `out` exactly "output full").
// Bytes produced, or -1.
long port_disc_unbzip2(const unsigned char* in, size_t inLen, unsigned char* out, size_t outCap)
{
    bz_stream s;
    int res;
    memset(&s, 0, sizeof s);
    if (BZ2_bzDecompressInit(&s, 0, 0) != BZ_OK)
        return -1;
    s.next_in = (char*)in;
    s.avail_in = (unsigned int)inLen;
    s.next_out = (char*)out;
    s.avail_out = (unsigned int)outCap;
    res = BZ2_bzDecompress(&s);
    BZ2_bzDecompressEnd(&s);
    if (res == BZ_STREAM_END || (res == BZ_OK && s.avail_out == 0))
        return (long)(outCap - s.avail_out);
    return -1;
}
