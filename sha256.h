#pragma once
#include <stdint.h>

#define SHA256_LENGTH 32

typedef struct {
	union {
		uint32_t h[8];
		uint8_t digest[32];
	};
	union {
		uint32_t w[16];
		uint8_t buffer[64];
	};
	size_t size;
	uint64_t totalSize;
} sha256_context;

extern "C"
{
	void sha256_starts(sha256_context* context);
	void sha256_update(sha256_context* context, const void* data, size_t length);
	void sha256_finish(sha256_context* context, uint8_t* digest);
}