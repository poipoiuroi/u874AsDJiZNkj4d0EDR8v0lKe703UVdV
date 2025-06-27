#include "include.h"
#include <filesystem>
#include <immintrin.h>

uint64_t get_playback_time(const player_t* mpctx)
{
	auto now = std::chrono::steady_clock::now();
	auto delta = now - mpctx->base_clock;
	return static_cast<uint64_t>(std::chrono::duration<double, std::milli>(delta).count());
}

void seek_to(player_t* mpctx, int64_t delta_ms)
{
	std::lock_guard<std::mutex> lock(mpctx->stream_mutex);

	uint64_t current_time = get_playback_time(mpctx);
	int64_t target_time = static_cast<int64_t>(current_time) + delta_ms;

	uint64_t video_duration = mpctx->video_track ? mpctx->video_track->duration * 1000ull / mpctx->video_track->timescale : 0;
	uint64_t audio_duration = mpctx->audio_track ? mpctx->audio_track->duration * 1000ull / mpctx->audio_track->timescale : 0;
	uint64_t max_duration = std::max(video_duration, audio_duration);

	if (target_time < 0) target_time = 0;
	if (static_cast<uint64_t>(target_time) > max_duration) target_time = max_duration;

	if (mpctx->video_track)
	{
		const auto& samples = mpctx->video_track->samples;
		size_t keyframe_idx = 0;
		for (size_t i = 0; i < samples.size(); ++i)
		{
			uint64_t pts_ms = samples[i].presentation_time * 1000ull / mpctx->video_track->timescale;
			if (pts_ms <= static_cast<uint64_t>(target_time) && samples[i].is_keyframe)
			{
				keyframe_idx = i;
			}
		}
		mpctx->vi_bias.store(keyframe_idx);
		printf("vi_bias: %zu\n", keyframe_idx);
	}

	if (mpctx->audio_track)
	{
		const auto& samples = mpctx->audio_track->samples;
		size_t audio_idx = 0;
		for (size_t i = 0; i < samples.size(); ++i)
		{
			uint64_t pts_ms = samples[i].presentation_time * 1000ull / mpctx->audio_track->timescale;
			if (pts_ms <= static_cast<uint64_t>(target_time))
			{
				audio_idx = i;
			}
			else
			{
				break;
			}
		}
		mpctx->au_bias.store(audio_idx);
		printf("au_bias: %zu\n", audio_idx);
	}

	mpctx->audio_frames.shutdown();
	mpctx->video_frames.shutdown();
	mpctx->play_aframes.shutdown();
	mpctx->play_vframes.shutdown();

	mpctx->audio_frames = std::move(safe_queue<player_t::audio_frame_t, 20>());
	mpctx->video_frames = std::move(safe_queue<player_t::video_frame_t, 20>());
	mpctx->play_aframes = std::move(safe_queue<player_t::audio_frame_t>());
	mpctx->play_vframes = std::move(safe_queue<player_t::video_frame_t>());

	mpctx->base_clock = std::chrono::steady_clock::now() - std::chrono::milliseconds(target_time);

	mpctx->can_proceed.store(true);
	mpctx->is_seeking.store(false);
}

void decode_video(player_t* mpctx)
{
	const auto& stream = mpctx->stream;
	const auto& video_samples = mpctx->video_track->samples;
	const uint8_t start_code[4] = { 0x00, 0x00, 0x00, 0x01 };

	std::vector<uint8_t> hevc_init_nalus;
	for (const auto& arr : mpctx->video_track->stsd->nal_units) {
		for (const auto& nal : arr) {
			hevc_init_nalus.insert(hevc_init_nalus.end(), start_code, start_code + 4);
			hevc_init_nalus.insert(hevc_init_nalus.end(), nal.begin(), nal.end());
		}
	}

	size_t last_index = mpctx->vi_bias.load();

	while (mpctx->state != player_t::STOPPED)
	{
		mpctx->video_decoded.store(false);

		de265_decoder_context* hevc_ctx = de265_new_decoder();
		if (!hevc_ctx) continue;

		de265_push_data(hevc_ctx, hevc_init_nalus.data(), hevc_init_nalus.size(), 0, 0);
		de265_flush_data(hevc_ctx);

		for (size_t i = last_index; i < video_samples.size(); ++i)
		{
			if (mpctx->state == player_t::STOPPED || mpctx->is_seeking.load()) break;

			const auto& sample = video_samples[i];
			std::vector<uint8_t> data(sample.size);

			{
				std::lock_guard<std::mutex> lock(mpctx->stream_mutex);
				stream->seekg(sample.file_offset);
				stream->read(reinterpret_cast<char*>(data.data()), data.size());
			}

			std::vector<uint8_t> annexb;
			size_t pos = 0;
			while (pos + 4 <= data.size()) {
				if (mpctx->state == player_t::STOPPED || mpctx->is_seeking.load()) break;

				uint32_t nal_len = (data[pos] << 24) | (data[pos + 1] << 16) | (data[pos + 2] << 8) | data[pos + 3];
				pos += 4;
				if (pos + nal_len > data.size()) break;
				annexb.insert(annexb.end(), start_code, start_code + 4);
				annexb.insert(annexb.end(), data.begin() + pos, data.begin() + pos + nal_len);
				pos += nal_len;
			}

			uint64_t pts = (sample.decode_time + sample.composition_offset) * 1000 / mpctx->video_track->timescale;
			de265_push_data(hevc_ctx, annexb.data(), annexb.size(), pts, 0);

			de265_error err;
			int more = 0;
			do {
				if (mpctx->state == player_t::STOPPED || mpctx->is_seeking.load()) break;

				err = de265_decode(hevc_ctx, &more);
				if (!de265_isOK(err)) break;

				const de265_image* img = nullptr;
				while ((img = de265_get_next_picture(hevc_ctx)) != nullptr)
				{
					if (mpctx->state == player_t::STOPPED || mpctx->is_seeking.load()) break;

					player_t::video_frame_t f;
					f.pts = de265_get_image_PTS(img);
					f.width = de265_get_image_width(img, 0);
					f.height = de265_get_image_height(img, 0);

					for (int c = 0; c < 3; ++c)
					{
						int stride;
						const uint8_t* plane = de265_get_image_plane(img, c, &stride);
						int pheight = de265_get_image_height(img, c);
						f.planes[c].assign(plane, plane + (stride * pheight));
						f.strides[c] = stride;
					}

					if (!mpctx->is_seeking.load()) mpctx->video_frames.push(std::move(f));
				}

			} while (more && mpctx->state != player_t::STOPPED && !mpctx->is_seeking.load());

			last_index = i + 1;
		}

		de265_free_decoder(hevc_ctx);
		mpctx->video_decoded.store(true);

		while (!mpctx->can_proceed.load()) std::this_thread::sleep_for(std::chrono::milliseconds(10));

		last_index = mpctx->vi_bias.load();
	}
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

void main_controller(player_t* mpctx)
{
	if (!mpctx->video_decoded.load() && !mpctx->audio_decoded.load() && mpctx->can_proceed.load())
		mpctx->can_proceed.store(false);

	if (!mpctx->audio_frames.size() && !mpctx->video_frames.size() &&
		!mpctx->play_aframes.size() && !mpctx->play_vframes.size() &&
		mpctx->audio_decoded.load() && mpctx->video_decoded.load() &&
		!mpctx->is_seeking.load())
	{
		mpctx->state.store(player_t::STOPPED);
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

	mpctx->state = player_t::PLAYING;
	mpctx->base_clock = std::chrono::steady_clock::now();

	std::jthread(decode_video, mpctx.get()).detach();
	std::jthread(decode_audio, mpctx.get()).detach();
	std::jthread(play_vframe, mpctx.get()).detach();
	std::jthread(play_aframe, mpctx.get()).detach();
	std::jthread(video_thread, mpctx.get()).detach();
	std::jthread(audio_thread, mpctx.get()).detach();

	button_t ck_pause(VK_SPACE, 150);
	button_t ck_vup(VK_UP, 100);
	button_t ck_vdown(VK_DOWN, 100);
	button_t ck_lseek(VK_LEFT, 300);
	button_t ck_rseek(VK_RIGHT, 300);

	while (mpctx->state != player_t::STOPPED)
	{
		main_controller(mpctx.get());

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

		if (ck_pause.is_pressed())
		{
			auto current = mpctx->state.load();

			if (current == player_t::PLAYING)
			{
				mpctx->pause_time = std::chrono::steady_clock::now();
				mpctx->state.store(player_t::PAUSED);
				SDL_PauseAudioStreamDevice(mpctx->audio_stream);
			}
			else if (current == player_t::PAUSED)
			{
				auto resume_time = std::chrono::steady_clock::now();
				auto paused_duration = resume_time - mpctx->pause_time;
				mpctx->base_clock += paused_duration;
				mpctx->state.store(player_t::PLAYING);
				SDL_ResumeAudioStreamDevice(mpctx->audio_stream);
			}
		}

		if (ck_vup.is_pressed())
		{
			float v = mpctx->volume.load();
			if (v < 3.0f) mpctx->volume.store(v + 0.1f);
		}

		if (ck_vdown.is_pressed())
		{
			float v = mpctx->volume.load();
			if (v > 0.0f) mpctx->volume.store(v - 0.1f);
		}

		if (ck_lseek.is_pressed())
		{
			seek_to(mpctx.get(), -1000);
		}

		if (ck_rseek.is_pressed())
		{
			seek_to(mpctx.get(), +1000);
		}

		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}

	SDL_Quit();

	return 0;
}