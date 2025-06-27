#pragma once
#ifndef _CRYPTOR_H_
#define _CRYPTOR_H_

#include "include.h"

class cryptor_t {
public:
	using T1 = const std::vector<uint8_t>&;
	using T2 = const std::wstring&;

	std::vector<uint8_t> encrypt_bin(T1 data, T1 key);
	std::vector<uint8_t> decrypt_bin(T1 data, T1 key);
	void encrypt_file(T2 ipath, T2 opath, T1 key);
	void decrypt_file(T2 ipath, T2 opath, T1 key);
	std::vector<uint8_t> b64_enc(T1 input);
	std::vector<uint8_t> b64_dec(T1 input);
	std::vector<uint8_t> sha256(const std::string& input);
	std::vector<uint8_t> sha256(const std::vector<char>& input);

private:
	void secure_clear(std::vector<uint8_t>& data)
	{
		volatile uint8_t* p = data.data();
		for (size_t i = 0; i < data.size(); ++i) p[i] = 0;
		data.clear();
	}
};

inline cryptor_t* g_cryptor()
{
	static cryptor_t cryptor;
	return &cryptor;
}

#endif