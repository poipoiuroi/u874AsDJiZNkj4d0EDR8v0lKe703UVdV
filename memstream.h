#pragma once
#ifndef _MEMSTREAM_H_
#define _MEMSTREAM_H_

#include "include.h"

class memstream_t
{
public:
	explicit memstream_t(const std::wstring& filepath, const std::vector<char>& password)
	{
		std::vector<uint8_t> encrypted;
		if (!read_file_to_vector(filepath, encrypted))
		{
			valid_ = false;
			return;
		}

		const auto hashed = g_cryptor()->sha256(password);
		SecureZeroMemory((PVOID)password.data(), password.size());

		std::vector<uint8_t> decrypted = g_cryptor()->decrypt_bin(encrypted, hashed);
		if (decrypted.empty())
		{
			valid_ = false;
			return;
		}

		buffer_ = std::move(decrypted);
		pos_ = 0;
		last_read_count_ = 0;
		valid_ = true;
	}

	~memstream_t()
	{
		SecureZeroMemory(buffer_.data(), buffer_.size());
	}

	bool is_valid() const { return valid_; }
	size_t size() const { return buffer_.size(); }

	bool seekg(size_t pos)
	{
		if (pos > buffer_.size()) return false;
		pos_ = pos;
		return true;
	}

	bool seekg(std::streamoff offset, std::ios_base::seekdir way)
	{
		std::streamoff new_pos = 0;
		switch (way)
		{
		case std::ios_base::beg:
			new_pos = offset;
			break;
		case std::ios_base::cur:
			new_pos = static_cast<std::streamoff>(pos_) + offset;
			break;
		case std::ios_base::end:
			new_pos = static_cast<std::streamoff>(buffer_.size()) + offset;
			break;
		default:
			return false;
		}

		if (new_pos < 0 || static_cast<size_t>(new_pos) > buffer_.size())
			return false;

		pos_ = static_cast<size_t>(new_pos);
		return true;
	}

	std::streampos tellg() const
	{
		return static_cast<std::streampos>(pos_);
	}

	std::streamsize gcount() const
	{
		return static_cast<std::streamsize>(last_read_count_);
	}

	void ignore(size_t n)
	{
		pos_ = std::min(pos_ + n, buffer_.size());
	}

	bool read(void* dst, size_t n)
	{
		if (pos_ + n > buffer_.size())
		{
			last_read_count_ = 0;
			return false;
		}
		std::memcpy(dst, buffer_.data() + pos_, n);
		pos_ += n;
		last_read_count_ = n;
		return true;
	}

private:
	std::vector<uint8_t> buffer_;
	size_t pos_ = 0;
	size_t last_read_count_ = 0;
	bool valid_ = false;

	bool read_file_to_vector(const std::wstring& path, std::vector<uint8_t>& out)
	{
		std::ifstream file(path, std::ios::binary | std::ios::ate);
		if (!file) return false;

		std::streamsize size = file.tellg();
		file.seekg(0, std::ios::beg);
		if (size <= 0) return false;

		out.resize(static_cast<size_t>(size));
		return file.read(reinterpret_cast<char*>(out.data()), size).good();
	}
};

#endif
