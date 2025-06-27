#pragma once
#pragma once
#ifndef _UTILS_H_
#define _UTILS_H_

#include "include.h"

template<typename T, size_t capacity = 0>
class safe_queue {
    std::queue<T> q;
    mutable std::mutex mtx;
    std::condition_variable cv_push;
    std::condition_variable cv_pop;
    std::atomic<bool> shutdown_flag{ false };
    std::atomic<bool> draining_flag{ false };

    static constexpr bool has_capacity = (capacity > 0);

public:
    safe_queue() = default;
    ~safe_queue() { shutdown(); }

    safe_queue(const safe_queue&) = delete;
    safe_queue& operator=(const safe_queue&) = delete;

    void push(T&& value)
    {
        std::unique_lock<std::mutex> lock(mtx);
        if constexpr (has_capacity) {
            cv_push.wait(lock, [&]() {
                return q.size() < capacity || shutdown_flag.load() || draining_flag.load();
                });
            if (shutdown_flag.load() || draining_flag.load()) return;
        }
        q.push(std::move(value));
        cv_pop.notify_one();
    }

    bool pop(T& result)
    {
        std::unique_lock<std::mutex> lock(mtx);
        cv_pop.wait(lock, [&]() {
            return !q.empty() || shutdown_flag.load();
            });

        if (shutdown_flag && q.empty()) return false;

        result = std::move(q.front());
        q.pop();
        if constexpr (has_capacity) cv_push.notify_one();
        return true;
    }

    size_t size() const
    {
        std::lock_guard<std::mutex> lock(mtx);
        return q.size();
    }

    void drain()
    {
        {
            std::lock_guard<std::mutex> lock(mtx);
            draining_flag.store(true);
            std::queue<T> empty;
            std::swap(q, empty);
        }
        cv_push.notify_all();
        draining_flag.store(false);
    }

    void shutdown() noexcept
    {
        shutdown_flag.store(true);
        cv_push.notify_all();
        cv_pop.notify_all();
    }

    safe_queue(safe_queue&& other) noexcept
    {
        std::scoped_lock lock(other.mtx);
        q = std::move(other.q);
        shutdown_flag.store(other.shutdown_flag.load());
        draining_flag.store(other.draining_flag.load());
    }

    safe_queue& operator=(safe_queue&& other) noexcept
    {
        if (this != &other)
        {
            std::scoped_lock lock(mtx, other.mtx);
            q = std::move(other.q);
            shutdown_flag.store(other.shutdown_flag.load());
            draining_flag.store(other.draining_flag.load());
        }
        return *this;
    }
};

class button_t
{
private:
	int vk_code;
	int delay;
	std::chrono::steady_clock::time_point last_time;

public:
	button_t(int keyCode, int ms) : vk_code(keyCode), delay(ms),
		last_time(std::chrono::steady_clock::now() - std::chrono::milliseconds(ms)) {
	}

	bool is_pressed()
	{
		if (GetAsyncKeyState(vk_code) & 0x8000)
		{
			auto now = std::chrono::steady_clock::now();
			auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_time);
			if (elapsed.count() >= delay)
			{
				last_time = now;
				return true;
			}
		}
		return false;
	}
};

inline uint16_t bswap16(uint16_t x)
{
	return ((x & 0xFF00) >> 8) |
		((x & 0x00FF) << 8);
}

inline uint32_t bswap32(uint32_t x)
{
	return ((x & 0xFF000000) >> 24) |
		((x & 0x00FF0000) >> 8) |
		((x & 0x0000FF00) << 8) |
		((x & 0x000000FF) << 24);
}

inline bool is_valid_atom_type(const char* type_buf)
{
	for (int i = 0; i < 4; ++i)
	{
		unsigned char c = static_cast<unsigned char>(type_buf[i]);
		if (c < 32 || c > 126) return false;
	}
	return true;
}

inline void zclear_console()
{
	HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
	CONSOLE_SCREEN_BUFFER_INFO csbi;
	DWORD written;
	GetConsoleScreenBufferInfo(h, &csbi);
	DWORD size = csbi.dwSize.X * csbi.dwSize.Y;
	FillConsoleOutputCharacter(h, ' ', size, { 0, 0 }, &written);
	FillConsoleOutputAttribute(h, csbi.wAttributes, size, { 0, 0 }, &written);
	SetConsoleCursorPosition(h, { 0, 0 });
}

inline bool read_password(std::vector<char>& password)
{
	HANDLE hi = GetStdHandle(STD_INPUT_HANDLE);
	if (hi == INVALID_HANDLE_VALUE) return false;

	DWORD br = 0;
	char data[256] = { 0 };

	if (!ReadFile(hi, data, 255, &br, NULL)) return false;

	while (br && (data[br - 1] == '\n' || data[br - 1] == '\r'))
		data[--br] = '\0';

	password.assign(data, data + br);
	SecureZeroMemory(data, sizeof(data));
	return true;
}

#endif