#include "include.h"
#include <filesystem>
#include <immintrin.h>

#include <fdk-aac/aacdecoder_lib.h>


uint64_t get_playback_time(const player_t* mpctx)
{
	auto now = std::chrono::steady_clock::now();
	auto delta = now - mpctx->base_clock;
	return static_cast<uint64_t>(std::chrono::duration<double, std::milli>(delta).count());
}

void decode_video(player_t* mpctx)
{
	const auto& stream = mpctx->stream;
	const auto& video_samples = mpctx->video_track->samples;
	const uint8_t start_code[4] = { 0x00, 0x00, 0x00, 0x01 };

	std::vector<uint8_t> hevc_init_nalus;
	for (const auto& arr : mpctx->video_track->stsd->nal_units)
	{
		for (const auto& nal : arr)
		{
			hevc_init_nalus.insert(hevc_init_nalus.end(), start_code, start_code + 4);
			hevc_init_nalus.insert(hevc_init_nalus.end(), nal.begin(), nal.end());
		}
	}

	const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_HEVC);
	if (!codec) return;

	AVCodecContext* codec_ctx = avcodec_alloc_context3(codec);
	if (!codec_ctx) return;

	if (avcodec_open2(codec_ctx, codec, nullptr) < 0)
	{
		avcodec_free_context(&codec_ctx);
		return;
	}

	AVPacket* pkt = av_packet_alloc();
	AVFrame* frame = av_frame_alloc();

	// Feed decoder configuration (VPS/SPS/PPS)
	{
		AVPacket* init_pkt = av_packet_alloc();
		init_pkt->data = hevc_init_nalus.data();
		init_pkt->size = static_cast<int>(hevc_init_nalus.size());
		avcodec_send_packet(codec_ctx, init_pkt);
		av_packet_free(&init_pkt);
	}

	size_t last_index = mpctx->vi_bias.load();

	while (mpctx->state != player_t::STOPPED)
	{
		mpctx->video_decoded.store(false);

		for (size_t i = last_index; i < video_samples.size(); ++i)
		{
			if (mpctx->state == player_t::STOPPED || mpctx->is_seeking.load()) break;

			const auto& sample = video_samples[i];
			std::vector<uint8_t> data(sample.size);

			{
				std::scoped_lock lock(mpctx->stream_mutex);
				stream->seekg(sample.file_offset);
				stream->read(reinterpret_cast<char*>(data.data()), data.size());
			}

			std::vector<uint8_t> nal_unit;
			nal_unit.insert(nal_unit.end(), start_code, start_code + 4);
			nal_unit.insert(nal_unit.end(), data.begin(), data.end());

			pkt->data = nal_unit.data();
			pkt->size = static_cast<int>(nal_unit.size());

			if (avcodec_send_packet(codec_ctx, pkt) == 0)
			{
				while (avcodec_receive_frame(codec_ctx, frame) == 0)
				{
					player_t::video_frame_t vframe{};
					vframe.pts = sample.decode_time * 1000ull / mpctx->video_track->timescale;
					vframe.width = frame->width;
					vframe.height = frame->height;

					int y_size = frame->width * frame->height;
					int uv_size = y_size / 4;

					vframe.planes[0].assign(frame->data[0], frame->data[0] + y_size);
					vframe.planes[1].assign(frame->data[1], frame->data[1] + uv_size);
					vframe.planes[2].assign(frame->data[2], frame->data[2] + uv_size);

					vframe.strides[0] = frame->linesize[0];
					vframe.strides[1] = frame->linesize[1];
					vframe.strides[2] = frame->linesize[2];

					if (!mpctx->is_seeking.load()) mpctx->video_frames.push(std::move(vframe));
				}
			}

			last_index = i + 1;
		}

		mpctx->video_decoded.store(true);
		while (!mpctx->can_proceed.load()) std::this_thread::sleep_for(std::chrono::milliseconds(10));
		last_index = mpctx->vi_bias.load();
	}

	av_frame_free(&frame);
	av_packet_free(&pkt);
	avcodec_free_context(&codec_ctx);
}

void decode_audio(player_t* mpctx)
{
	const auto& stream = mpctx->stream;
	const auto& audio_samples = mpctx->audio_track->samples;

	size_t last_index = mpctx->au_bias.load();

	while (mpctx->state != player_t::STOPPED)
	{
		mpctx->audio_decoded.store(false);

		HANDLE_AACDECODER aac_decoder = aacDecoder_Open(TT_MP4_RAW, 1);
		if (!aac_decoder) continue;

		{
			auto& asc = mpctx->audio_track->stsd->asc_bytes;
			auto ascLen = static_cast<UINT>(asc.size());
			UCHAR* ascData = asc.data();
			aacDecoder_ConfigRaw(aac_decoder, &ascData, &ascLen);
		}

		uint64_t decoded_sample_count = 0;

		for (size_t i = last_index; i < audio_samples.size(); ++i)
		{
			if (mpctx->state == player_t::STOPPED || mpctx->is_seeking.load()) break;

			const auto& sample = audio_samples[i];
			std::vector<uint8_t> data(sample.size);

			{
				std::scoped_lock lock(mpctx->stream_mutex);
				stream->seekg(sample.file_offset);
				stream->read(reinterpret_cast<char*>(data.data()), data.size());
			}

			UCHAR* ptr = data.data();
			UINT buffer_size = data.size();
			UINT bytes_valid = data.size();

			if (aacDecoder_Fill(aac_decoder, &ptr, &buffer_size, &bytes_valid) == AAC_DEC_OK && !mpctx->is_seeking.load())
			{
				std::vector<int16_t> pcm(2048 * 2 * 2);
				if (aacDecoder_DecodeFrame(aac_decoder, pcm.data(), pcm.size(), 0) == AAC_DEC_OK && !mpctx->is_seeking.load())
				{
					const CStreamInfo* info = aacDecoder_GetStreamInfo(aac_decoder);
					if (info && info->sampleRate && info->numChannels && !mpctx->is_seeking.load())
					{
						pcm.resize(info->frameSize * info->numChannels);

						player_t::audio_frame_t s;
						s.sample_rate = info->sampleRate;
						s.channels = info->numChannels;
						s.frame_size = info->frameSize;
						s.pts = sample.decode_time * 1000ull / mpctx->audio_track->timescale;
						s.pcm = std::move(pcm);

						decoded_sample_count += info->frameSize;
						if (!mpctx->is_seeking.load()) mpctx->audio_frames.push(std::move(s));
					}
				}
			}

			last_index = i + 1;
		}

		aacDecoder_Close(aac_decoder);
		mpctx->audio_decoded.store(true);

		while (!mpctx->can_proceed.load()) std::this_thread::sleep_for(std::chrono::milliseconds(10));

		last_index = mpctx->au_bias.load();
	}
}

void play_vframe(player_t* mpctx)
{
	while (mpctx->state != player_t::STOPPED)
	{
		if (mpctx->state == player_t::PAUSED)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
			continue;
		}

		if (mpctx->is_seeking.load())
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
			continue;
		}

		player_t::video_frame_t frame{};
		if (!mpctx->play_vframes.pop(frame)) continue;

		SDL_UpdateYUVTexture(mpctx->texture, 0,
			frame.planes[0].data(), frame.strides[0],
			frame.planes[1].data(), frame.strides[1],
			frame.planes[2].data(), frame.strides[2]);

		SDL_RenderClear(mpctx->renderer);
		SDL_RenderTexture(mpctx->renderer, mpctx->texture, 0, 0);
		SDL_RenderPresent(mpctx->renderer);

		SDL_Event e;
		while (SDL_PollEvent(&e))
		{
			if (e.type == SDL_EVENT_QUIT) exit(0);
		}
	}
}

void play_aframe(player_t* mpctx)
{
	while (mpctx->state != player_t::STOPPED)
	{
		if (mpctx->state == player_t::PAUSED)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
			continue;
		}

		if (mpctx->is_seeking.load())
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
			continue;
		}

		player_t::audio_frame_t frame{};
		if (!mpctx->play_aframes.pop(frame)) continue;

		float volume = mpctx->volume.load();
		if (volume == 0.0f)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
			continue;
		}

		int16_t* pcm = frame.pcm.data();
		size_t count = frame.pcm.size();

		if (volume != 1.0f)
		{
			__m256 vVolume = _mm256_set1_ps(volume);
			__m256i vMin = _mm256_set1_epi32(INT16_MIN);
			__m256i vMax = _mm256_set1_epi32(INT16_MAX);

			size_t i = 0;
			for (; i + 15 < count; i += 16) {
				__m256i in = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(pcm + i));

				__m128i inLo = _mm256_extracti128_si256(in, 0);
				__m128i inHi = _mm256_extracti128_si256(in, 1);

				__m256i inLo32 = _mm256_cvtepi16_epi32(inLo);
				__m256i inHi32 = _mm256_cvtepi16_epi32(inHi);

				__m256 fLo = _mm256_cvtepi32_ps(inLo32);
				__m256 fHi = _mm256_cvtepi32_ps(inHi32);

				fLo = _mm256_mul_ps(fLo, vVolume);
				fHi = _mm256_mul_ps(fHi, vVolume);

				__m256i sLo32 = _mm256_cvtps_epi32(fLo);
				__m256i sHi32 = _mm256_cvtps_epi32(fHi);

				sLo32 = _mm256_max_epi32(vMin, _mm256_min_epi32(vMax, sLo32));
				sHi32 = _mm256_max_epi32(vMin, _mm256_min_epi32(vMax, sHi32));

				__m128i packedLo = _mm_packs_epi32(
					_mm256_extracti128_si256(sLo32, 0),
					_mm256_extracti128_si256(sLo32, 1)
				);

				__m128i packedHi = _mm_packs_epi32(
					_mm256_extracti128_si256(sHi32, 0),
					_mm256_extracti128_si256(sHi32, 1)
				);

				__m256i result = _mm256_insertf128_si256(_mm256_castsi128_si256(packedLo), (packedHi), 0x1);
				_mm256_storeu_si256(reinterpret_cast<__m256i*>(pcm + i), result);
			}

			for (; i < count; ++i)
			{
				int sample = static_cast<int>(pcm[i] * volume);
				pcm[i] = static_cast<int16_t>(std::max(std::min(sample, static_cast<int>(INT16_MAX)), static_cast<int>(INT16_MIN)));
			}
		}

		SDL_PutAudioStreamData(mpctx->audio_stream, pcm, static_cast<int>(count * sizeof(int16_t)));
	}
}

void video_thread(player_t* mpctx)
{
	player_t::video_frame_t pending_frame{};
	bool has_pending = false;

	while (mpctx->state != player_t::STOPPED)
	{
		if (mpctx->state == player_t::PAUSED)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
			continue;
		}

		if (mpctx->is_seeking.load())
		{
			has_pending = false;
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
			continue;
		}

		if (has_pending)
		{
			uint64_t now = get_playback_time(mpctx);
			if (now >= pending_frame.pts)
			{
				mpctx->play_vframes.push(std::move(pending_frame));
				has_pending = false;
			}
			else
			{
				auto wait_ms = pending_frame.pts > now ? pending_frame.pts - now : 0;
				std::this_thread::sleep_for(std::chrono::milliseconds(std::min<uint64_t>(wait_ms, 5)));
				continue;
			}
		}
		else
		{
			player_t::video_frame_t frame{};
			if (mpctx->video_frames.pop(frame))
			{
				pending_frame = std::move(frame);
				has_pending = true;
			}
			else
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
		}
	}
}

void audio_thread(player_t* mpctx)
{
	player_t::audio_frame_t pending_frame{};
	bool has_pending = false;

	while (mpctx->state != player_t::STOPPED)
	{
		if (mpctx->state == player_t::PAUSED)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
			continue;
		}

		if (mpctx->is_seeking.load())
		{
			has_pending = false;
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
			continue;
		}

		if (has_pending)
		{
			uint64_t now = get_playback_time(mpctx);
			if (now >= pending_frame.pts)
			{
				mpctx->play_aframes.push(std::move(pending_frame));
				has_pending = false;
			}
			else
			{
				auto wait_ms = pending_frame.pts > now ? pending_frame.pts - now : 0;
				std::this_thread::sleep_for(std::chrono::milliseconds(std::min<uint64_t>(wait_ms, 5)));
			}
		}
		else
		{
			player_t::audio_frame_t frame{};
			if (mpctx->audio_frames.pop(frame))
			{
				pending_frame = std::move(frame);
				has_pending = true;
			}
		}
	}
}

int wmain(int argc, wchar_t** argv)
{
	/*if (argc < 2 || !argv[1][0])
		return 1;*/

	std::vector<char> password{ 'z', '1', 'x', '2', 'c', '3', 'v', '4', 'b', '5', 'n', '6' };// (256);

	/*if (!read_password(password))
		return 1;*/

	if (password.empty())
		return 2;

	std::wstring filepath = L"C:\\C\\1_x265_enc.mp4";// argv[1];
	if (!std::filesystem::exists(filepath))
		return 3;

	zclear_console();

	std::unique_ptr<player_t> mpctx = std::make_unique<player_t>();

	mpctx->stream = std::make_unique<memstream_t>(filepath, password);
	if (!mpctx->stream->is_valid())
		return 4;

	mpctx->mp4 = std::make_unique<mp4_t>();
	if (!mpctx->mp4->parse(mpctx->stream.get()))
		return 5;

	for (const auto& track : mpctx->mp4->_tracks)
	{
		if (!mpctx->video_track && track.type == 'vide')
			mpctx->video_track = &track;

		if (!mpctx->audio_track && track.type == 'soun')
			mpctx->audio_track = &track;
	}

	if (!mpctx->video_track || !mpctx->audio_track)
		return 6;

	if (mpctx->video_track && mpctx->video_track->stsd->nal_units.empty())
		return 7;

	if (mpctx->audio_track && mpctx->audio_track->stsd->asc_bytes.empty())
		return 8;

	{
		if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) < 0) return 6;

		mpctx->window = SDL_CreateWindow("playa", mpctx->video_track->width / 1.5, mpctx->video_track->height / 1.5, SDL_WINDOW_RESIZABLE);
		if (!mpctx->window) return 8;

		mpctx->renderer = SDL_CreateRenderer(mpctx->window, 0);
		if (!mpctx->renderer) return 9;

		mpctx->texture = SDL_CreateTexture(mpctx->renderer, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, mpctx->video_track->width, mpctx->video_track->height);
		if (!mpctx->texture) return 10;

		SDL_AudioSpec audio_spec{};
		audio_spec.freq = mpctx->audio_track->sample_rate;
		audio_spec.channels = mpctx->audio_track->channel_count;
		audio_spec.format = SDL_AUDIO_S16; // mpctx->audio_track->sample_size

		mpctx->audio_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &audio_spec, 0, 0);
		if (!mpctx->audio_stream)
			return 11;

		SDL_ResumeAudioStreamDevice(mpctx->audio_stream);
	}

	mpctx->base_clock = std::chrono::steady_clock::now();
	mpctx->state = player_t::PLAYING;

	std::jthread(decode_video, mpctx.get()).detach();
	std::jthread(decode_audio, mpctx.get()).detach();
	std::jthread(play_vframe, mpctx.get()).detach();
	std::jthread(play_aframe, mpctx.get()).detach();
	std::jthread(video_thread, mpctx.get()).detach();
	std::jthread(audio_thread, mpctx.get()).detach();

	while (mpctx->state != player_t::STOPPED)
	{
		SDL_Event e;
		while (SDL_PollEvent(&e))
		{
			switch (e.type)
			{
			case SDL_EVENT_QUIT:
				mpctx->state.store(player_t::STOPPED);
				break;
			case SDL_EVENT_WINDOW_MOVED:
			case SDL_EVENT_WINDOW_RESIZED:
			case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
			case SDL_EVENT_WINDOW_RESTORED:
				break;
			}
		}

		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}

	SDL_Quit();

	return 0;
}

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

	enum state_t { PLAYING, PAUSED, STOPPED };
	std::atomic<state_t> state = STOPPED;

	std::mutex stream_mutex;
	std::unique_ptr<memstream_t> stream;

	std::unique_ptr<mp4_t> mp4;

	safe_queue<video_frame_t, 20> video_frames;
	safe_queue<audio_frame_t, 20> audio_frames;

	safe_queue<player_t::video_frame_t> play_vframes;
	safe_queue<player_t::audio_frame_t> play_aframes;

	const track_t* video_track = 0;
	const track_t* audio_track = 0;

	std::chrono::steady_clock::time_point pause_time;
	std::chrono::steady_clock::time_point base_clock;

	std::atomic<bool> audio_decoded = false;
	std::atomic<bool> video_decoded = false;
	std::atomic<bool> can_proceed = false;

	std::atomic<bool> is_seeking = false;
	std::atomic<size_t> vi_bias = 0;
	std::atomic<size_t> au_bias = 0;

	std::atomic<float> volume{ 1.0f };

	SDL_Window* window = 0;
	SDL_Renderer* renderer = 0;
	SDL_Texture* texture = 0;
	SDL_AudioStream* audio_stream = 0;
};