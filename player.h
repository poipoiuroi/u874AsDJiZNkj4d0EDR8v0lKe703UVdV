#pragma once
#ifndef _MP4_H_
#define _MP4_H_

#include "include.h"

struct atom_t
{
	atom_t(uint64_t offset, uint64_t size, const std::string& type) : _offset(offset), _size(size), _type(type) {}

	uint64_t _offset = 0;
	uint64_t _size = 0;
	std::string _type;
	std::vector<std::unique_ptr<atom_t>> _children;

	virtual void parse(memstream_t* stream);
	static std::unique_ptr<atom_t> create_atom(uint64_t offset, uint64_t size, const std::string& type);
};

struct mdhd_atom_t : public atom_t
{
	mdhd_atom_t(uint64_t offset, uint64_t size, const std::string& type) : atom_t(offset, size, type) {}

	uint32_t timescale = 0;
	uint32_t duration = 0;

	void parse(memstream_t* stream) override;
};

struct hdlr_atom_t : public atom_t
{
	hdlr_atom_t(uint64_t offset, uint64_t size, const std::string& type) : atom_t(offset, size, type) {}

	uint32_t type = 0;

	void parse(memstream_t* stream) override;
};

struct stsz_atom_t : public atom_t
{
	stsz_atom_t(uint64_t offset, uint64_t size, const std::string& type) : atom_t(offset, size, type) {}

	std::vector<uint32_t> entries;

	void parse(memstream_t* stream) override;
};

struct stsc_atom_t : public atom_t
{
	stsc_atom_t(uint64_t offset, uint64_t size, const std::string& type) : atom_t(offset, size, type) {}

	struct entry_t
	{
		uint32_t first_chunk;
		uint32_t samples_per_chunk;
		uint32_t sample_description_index;
	};

	std::vector<entry_t> entries;

	void parse(memstream_t* stream) override;
};

struct stco_atom_t : public atom_t
{
	stco_atom_t(uint64_t offset, uint64_t size, const std::string& type) : atom_t(offset, size, type) {}

	std::vector<uint32_t> entries;

	void parse(memstream_t* stream) override;
};

struct stts_atom_t : public atom_t
{
	stts_atom_t(uint64_t offset, uint64_t size, const std::string& type) : atom_t(offset, size, type) {}

	struct entry_t
	{
		uint32_t sample_count;
		uint32_t sample_delta;
	};

	std::vector<entry_t> entries;

	void parse(memstream_t* stream) override;
};

struct ctts_atom_t : public atom_t
{
	ctts_atom_t(uint64_t offset, uint64_t size, const std::string& type) : atom_t(offset, size, type) {}

	struct entry_t
	{
		uint32_t sample_count;
		uint32_t sample_offset;
	};

	std::vector<entry_t> entries;

	void parse(memstream_t* stream) override;
};

struct stss_atom_t : public atom_t
{
	stss_atom_t(uint64_t offset, uint64_t size, const std::string& type) : atom_t(offset, size, type) {}

	std::vector<uint32_t> entries;

	void parse(memstream_t* stream) override;
};

struct stsd_atom_t : public atom_t
{
	stsd_atom_t(uint64_t offset, uint64_t size, const std::string& type) : atom_t(offset, size, type) {}

	std::vector<std::vector<std::vector<uint8_t>>> nal_units;
	std::vector<uint8_t> asc_bytes;

	uint32_t width = 0;
	uint32_t height = 0;
	uint16_t channel_count = 0;
	uint16_t sample_size = 0;
	float sample_rate = 0;

	uint32_t type = 0;

	void parse(memstream_t* stream, uint32_t htype);
};

struct sample_t
{
	uint32_t duration = 0;
	uint64_t file_offset = 0;
	uint32_t size = 0;
	uint32_t decode_time = 0;
	uint32_t composition_offset = 0;
	uint64_t presentation_time = 0;
	bool is_keyframe = true;
};

struct track_t
{
	uint32_t type = 0;
	uint32_t timescale = 0;
	uint64_t duration = 0;

	uint32_t width = 0;
	uint32_t height = 0;

	uint32_t channel_count = 0;
	uint32_t sample_rate = 0;
	uint32_t sample_size = 0;

	stsd_atom_t* stsd = 0;
	stts_atom_t* stts = 0;
	stss_atom_t* stss = 0;
	ctts_atom_t* ctts = 0;
	stsc_atom_t* stsc = 0;
	stsz_atom_t* stsz = 0;
	stco_atom_t* stco = 0;

	std::vector<sample_t> samples;
};

struct mp4_t
{
	bool parse(memstream_t* stream);
	void build_samples(track_t& track);

	std::vector<track_t> _tracks;
	std::vector<std::unique_ptr<atom_t>> _atoms;
};

struct player_t
{
	struct video_frame_t
	{
		uint64_t pts = 0;
		int width = 0;
		int height = 0;
		std::array<std::vector<uint8_t>, 3> planes{};
		std::array<int, 3> strides{};
	};

	struct audio_frame_t
	{
		uint64_t pts = 0;
		int sample_rate = 0;
		int channels = 0;
		int frame_size = 0;
		std::vector<int16_t> pcm;
	};

	SDL_Window* window = 0;
	SDL_Renderer* renderer = 0;
	SDL_Texture* texture = 0;
	SDL_AudioStream* audio_stream = 0;

	enum state_t { PLAYING, PAUSED, STOPPED, SEEKING };
	std::atomic<state_t> state = STOPPED;

	std::unique_ptr<mp4_t> mp4;

	std::mutex stream_mutex;
	std::unique_ptr<memstream_t> stream;

	const track_t* video_track = 0;
	const track_t* audio_track = 0;

	safe_queue<video_frame_t, 20> video_frames;
	safe_queue<audio_frame_t, 20> audio_frames;

	safe_queue<player_t::video_frame_t> play_vframes;
	safe_queue<player_t::audio_frame_t> play_aframes;

	std::chrono::steady_clock::time_point pause_time;
	std::chrono::steady_clock::time_point base_clock;

	std::atomic<size_t> v_idx{ 0 };
	std::atomic<size_t> a_idx{ 0 };

	std::atomic<bool> dec_audiof{ false };
	std::atomic<bool> dec_videof{ false };

	std::atomic<float> volume{ 1.0f };
};

#endif