#pragma once
#include <stdint.h>

#define AES_BLOCK_SIZE 16

typedef struct {
	unsigned int roundkey[60];
	unsigned int iv[4];
} AES_CTX;

extern "C"
{
	void AES_EncryptInit(AES_CTX* ctx, const uint8_t* key, const uint8_t* iv);
	void AES_DecryptInit(AES_CTX* ctx, const uint8_t* key, const uint8_t* iv);
	void AES_Encrypt(AES_CTX* ctx, const uint8_t in_data[AES_BLOCK_SIZE], uint8_t out_data[AES_BLOCK_SIZE]);
	void AES_Decrypt(AES_CTX* ctx, const uint8_t in_data[AES_BLOCK_SIZE], uint8_t out_data[AES_BLOCK_SIZE]);
	void AES_CTX_Free(AES_CTX* ctx);
}