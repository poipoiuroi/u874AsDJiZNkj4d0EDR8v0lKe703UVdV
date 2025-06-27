#include "sha256.h"
#include <vcruntime_string.h>


#define W(n) w[(n) & 0x0F]
#define CH(x, y, z) (((x) & (y)) | (~(x) & (z)))
#define MAJ(x, y, z) (((x) & (y)) | ((x) & (z)) | ((y) & (z)))
#define SHR32(x, n) ((x) >> (n))
#define ROR32(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define SIGMA1(x) (ROR32(x, 2) ^ ROR32(x, 13) ^ ROR32(x, 22))
#define SIGMA2(x) (ROR32(x, 6) ^ ROR32(x, 11) ^ ROR32(x, 25))
#define SIGMA3(x) (ROR32(x, 7) ^ ROR32(x, 18) ^ SHR32(x, 3))
#define SIGMA4(x) (ROR32(x, 17) ^ ROR32(x, 19) ^ SHR32(x, 10))
#define MIN(a, b) ((a) < (b) ? (a) : (b))

#define SWAPINT32(x) ( \
    (((uint32_t)(x) & 0x000000FFUL) << 24) | \
    (((uint32_t)(x) & 0x0000FF00UL) << 8) | \
    (((uint32_t)(x) & 0x00FF0000UL) >> 8) | \
    (((uint32_t)(x) & 0xFF000000UL) >> 24))

#define htobe32(value) SWAPINT32((uint32_t) (value))

static const uint8_t padding[64] = {
   0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
   0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
   0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
   0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

static const uint32_t k[64] = {
   0x428A2F98, 0x71374491, 0xB5C0FBCF, 0xE9B5DBA5, 0x3956C25B, 0x59F111F1, 0x923F82A4, 0xAB1C5ED5,
   0xD807AA98, 0x12835B01, 0x243185BE, 0x550C7DC3, 0x72BE5D74, 0x80DEB1FE, 0x9BDC06A7, 0xC19BF174,
   0xE49B69C1, 0xEFBE4786, 0x0FC19DC6, 0x240CA1CC, 0x2DE92C6F, 0x4A7484AA, 0x5CB0A9DC, 0x76F988DA,
   0x983E5152, 0xA831C66D, 0xB00327C8, 0xBF597FC7, 0xC6E00BF3, 0xD5A79147, 0x06CA6351, 0x14292967,
   0x27B70A85, 0x2E1B2138, 0x4D2C6DFC, 0x53380D13, 0x650A7354, 0x766A0ABB, 0x81C2C92E, 0x92722C85,
   0xA2BFE8A1, 0xA81A664B, 0xC24B8B70, 0xC76C51A3, 0xD192E819, 0xD6990624, 0xF40E3585, 0x106AA070,
   0x19A4C116, 0x1E376C08, 0x2748774C, 0x34B0BCB5, 0x391C0CB3, 0x4ED8AA4A, 0x5B9CCA4F, 0x682E6FF3,
   0x748F82EE, 0x78A5636F, 0x84C87814, 0x8CC70208, 0x90BEFFFA, 0xA4506CEB, 0xBEF9A3F7, 0xC67178F2
};

void sha256_process(sha256_context* context)
{
	unsigned int i;
	uint32_t temp1, temp2;
	uint32_t a = context->h[0];
	uint32_t b = context->h[1];
	uint32_t c = context->h[2];
	uint32_t d = context->h[3];
	uint32_t e = context->h[4];
	uint32_t f = context->h[5];
	uint32_t g = context->h[6];
	uint32_t h = context->h[7];
	uint32_t* w = context->w;

	for (i = 0; i < 16; i++) w[i] = htobe32(w[i]);

	for (i = 0; i < 64; i++)
	{
		if (i >= 16) W(i) += SIGMA4(W(i + 14)) + W(i + 9) + SIGMA3(W(i + 1));
		temp1 = h + SIGMA2(e) + CH(e, f, g) + k[i] + W(i);
		temp2 = SIGMA1(a) + MAJ(a, b, c);

		h = g;
		g = f;
		f = e;
		e = d + temp1;
		d = c;
		c = b;
		b = a;
		a = temp1 + temp2;
	}

	context->h[0] += a;
	context->h[1] += b;
	context->h[2] += c;
	context->h[3] += d;
	context->h[4] += e;
	context->h[5] += f;
	context->h[6] += g;
	context->h[7] += h;
}

void sha256_starts(sha256_context* context)
{
	context->size = 0;
	context->totalSize = 0;
	context->h[0] = 0x6A09E667;
	context->h[1] = 0xBB67AE85;
	context->h[2] = 0x3C6EF372;
	context->h[3] = 0xA54FF53A;
	context->h[4] = 0x510E527F;
	context->h[5] = 0x9B05688C;
	context->h[6] = 0x1F83D9AB;
	context->h[7] = 0x5BE0CD19;
}

void sha256_update(sha256_context* context, const void* data, size_t length)
{
	size_t n;
	while (length > 0) {
		n = MIN(length, 64 - context->size);
		memcpy(context->buffer + context->size, data, n);
		context->size += n;
		context->totalSize += n;
		data = (uint8_t*)data + n;
		length -= n;
		if (context->size == 64) {
			sha256_process(context);
			context->size = 0;
		}
	}
}

void sha256_finish(sha256_context* context, uint8_t* digest)
{
	unsigned int i;
	size_t paddingSize;
	uint64_t totalSize;

	totalSize = context->totalSize * 8;
	if (context->size < 56) paddingSize = 56 - context->size;
	else paddingSize = 64 + 56 - context->size;

	sha256_update(context, padding, paddingSize);
	context->w[14] = htobe32((uint32_t)(totalSize >> 32));
	context->w[15] = htobe32((uint32_t)totalSize);

	sha256_process(context);
	for (i = 0; i < 8; i++) context->h[i] = htobe32(context->h[i]);
	if (digest != 0) memcpy(digest, context->digest, 32);
}