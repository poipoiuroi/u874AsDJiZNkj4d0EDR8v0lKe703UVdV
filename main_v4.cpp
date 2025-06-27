#include "include.h"

__forceinline void sleep_for(uint64_t ms)
{
	std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}
uint64_t get_playback_time(player_t* mpctx)
{
	auto now = std::chrono::steady_clock::now();
	auto delta = now - mpctx->base_clock;
	return static_cast<uint64_t>(std::chrono::duration<double, std::milli>(delta).count());
}

void handle_seek(player_t* mpctx, int64_t delta_ms)
{
	if (!mpctx || !mpctx->video_track || !mpctx->audio_track) return;

	mpctx->state.store(player_t::SEEKING);

	int64_t current_time = static_cast<int64_t>(get_playback_time(mpctx));
	int64_t target_time = std::max<int64_t>(0, current_time + delta_ms);
	std::lock_guard<std::mutex> lock(mpctx->stream_mutex);

	mpctx->video_frames.drain();
	mpctx->audio_frames.drain();
	mpctx->play_vframes.drain();
	mpctx->play_aframes.drain();

	const auto& v_samples = mpctx->video_track->samples;
	const auto& a_samples = mpctx->audio_track->samples;

	auto find_sample_idx = [](const std::vector<sample_t>& samples, uint64_t target_ms, uint32_t timescale) -> size_t {
		if (samples.empty()) return 0;

		uint64_t target_pts = static_cast<uint64_t>((target_ms / 1000.0) * timescale);
		for (size_t i = samples.size(); i-- > 0;) {
			if (samples[i].presentation_time <= target_pts)
				return i;
		}
		return 0;
		};

	mpctx->v_idx.store(find_sample_idx(v_samples, target_time, mpctx->video_track->timescale));
	mpctx->a_idx.store(find_sample_idx(a_samples, target_time, mpctx->audio_track->timescale));

	sleep_for(1000);
	mpctx->base_clock = std::chrono::steady_clock::now() - std::chrono::milliseconds(target_time);
	mpctx->state.store(player_t::PLAYING);
}

void decode_video(player_t* mpctx)
{
	const auto& v_samples = mpctx->video_track->samples;

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

	while (true)
	{
		auto state = mpctx->state.load();

		if (state == player_t::STOPPED)
			break;

		static bool once = true;
		if (state == player_t::PAUSED || state == player_t::SEEKING)
		{ 
			if (once) { printf("decode_video\n"); once = false; }
			sleep_for(10);
			continue;
		}
		once = true;

		size_t idx = mpctx->v_idx.load();
		if (idx >= v_samples.size())
		{
			sleep_for(10);
			continue;
		}

		de265_decoder_context* decoder = de265_new_decoder();
		if (!decoder)
			continue;

		de265_push_data(decoder, hevc_init_nalus.data(), hevc_init_nalus.size(), 0, 0);
		de265_flush_data(decoder);

		mpctx->dec_videof.store(false);

		for (; idx < v_samples.size(); ++idx)
		{
			state = mpctx->state.load();
			if (state == player_t::STOPPED || state == player_t::SEEKING)
				break;

			const auto& sample = v_samples[idx];
			std::vector<uint8_t> data(sample.size);
			{
				std::lock_guard<std::mutex> lock(mpctx->stream_mutex);
				mpctx->stream->seekg(sample.file_offset);
				mpctx->stream->read(data.data(), data.size());
			}

			std::vector<uint8_t> annexb;
			size_t pos = 0;
			while (pos + 4 <= data.size())
			{
				state = mpctx->state.load();
				if (state == player_t::STOPPED || state == player_t::SEEKING)
					break;

				uint32_t nal_len = (data[pos] << 24) | (data[pos + 1] << 16) | (data[pos + 2] << 8) | data[pos + 3];
				pos += 4;
				if (pos + nal_len > data.size()) break;
				annexb.insert(annexb.end(), start_code, start_code + 4);
				annexb.insert(annexb.end(), data.begin() + pos, data.begin() + pos + nal_len);
				pos += nal_len;
			}

			uint64_t pts = (sample.decode_time + sample.composition_offset) * 1000 / mpctx->video_track->timescale;
			de265_push_data(decoder, annexb.data(), annexb.size(), pts, 0);

			de265_error err;
			int more = 0;
			do
			{
				state = mpctx->state.load();
				if (state == player_t::STOPPED || state == player_t::SEEKING)
					break;

				err = de265_decode(decoder, &more);
				if (!de265_isOK(err)) break;

				const de265_image* img = 0;
				while ((img = de265_get_next_picture(decoder)) != 0)
				{
					state = mpctx->state.load();
					if (state == player_t::STOPPED || state == player_t::SEEKING)
						break;

					player_t::video_frame_t f{};
					f.pts = de265_get_image_PTS(img);
					f.width = de265_get_image_width(img, 0);
					f.height = de265_get_image_height(img, 0);

					for (int c = 0; c < 3; ++c)
					{
						int stride;
						const uint8_t* plane = de265_get_image_plane(img, c, &stride);
						int h = de265_get_image_height(img, c);
						f.planes[c].assign(plane, plane + (stride * h));
						f.strides[c] = stride;
					}

					mpctx->video_frames.push(std::move(f));
				}

			} while (more);

			mpctx->v_idx.fetch_add(1);
		}

		mpctx->dec_videof.store(true);
	}
}

void decode_audio(player_t* mpctx)
{
	const auto& a_samples = mpctx->audio_track->samples;

	while (true)
	{
		auto state = mpctx->state.load();

		if (state == player_t::STOPPED)
			break;

		static bool once = true;
		if (state == player_t::PAUSED || state == player_t::SEEKING)
		{
			if (once) { printf("decode_audio\n"); once = false; }
			sleep_for(10);
			continue;
		}
		once = true;


		size_t idx = mpctx->v_idx.load();
		if (idx >= a_samples.size())
		{
			sleep_for(10);
			continue;
		}

		HANDLE_AACDECODER aac_decoder = aacDecoder_Open(TT_MP4_RAW, 1);
		if (!aac_decoder) continue;

		{
			auto& asc = mpctx->audio_track->stsd->asc_bytes;
			auto ascLen = static_cast<UINT>(asc.size());
			UCHAR* ascData = asc.data();
			aacDecoder_ConfigRaw(aac_decoder, &ascData, &ascLen);
		}

		uint64_t decoded_sample_count = 0;

		mpctx->dec_audiof.store(false);

		for (; idx < a_samples.size(); ++idx)
		{
			state = mpctx->state.load();
			if (state == player_t::STOPPED || state == player_t::SEEKING)
				break;

			const auto& sample = a_samples[idx];
			std::vector<uint8_t> data(sample.size);
			{
				std::lock_guard<std::mutex> lock(mpctx->stream_mutex);
				mpctx->stream->seekg(sample.file_offset);
				mpctx->stream->read(data.data(), data.size());
			}

			UCHAR* ptr = data.data();
			UINT buffer_size = data.size();
			UINT bytes_valid = data.size();

			if (aacDecoder_Fill(aac_decoder, &ptr, &buffer_size, &bytes_valid) == AAC_DEC_OK)
			{
				state = mpctx->state.load();
				if (state == player_t::STOPPED || state == player_t::SEEKING)
					break;

				std::vector<int16_t> pcm(2048 * 2 * 2);
				if (aacDecoder_DecodeFrame(aac_decoder, pcm.data(), pcm.size(), 0) == AAC_DEC_OK)
				{
					state = mpctx->state.load();
					if (state == player_t::STOPPED || state == player_t::SEEKING)
						break;

					const CStreamInfo* info = aacDecoder_GetStreamInfo(aac_decoder);
					if (info && info->sampleRate && info->numChannels)
					{
						state = mpctx->state.load();
						if (state == player_t::STOPPED || state == player_t::SEEKING)
							break;

						pcm.resize(info->frameSize * info->numChannels);

						player_t::audio_frame_t s{};
						s.sample_rate = info->sampleRate;
						s.channels = info->numChannels;
						s.frame_size = info->frameSize;
						s.pts = sample.decode_time * 1000ull / mpctx->audio_track->timescale;
						s.pcm = std::move(pcm);

						decoded_sample_count += info->frameSize;
						mpctx->audio_frames.push(std::move(s));
					}
				}
			}

			mpctx->v_idx.fetch_add(1);
		}

		mpctx->dec_audiof.store(true);
	}
}

void video_frame(player_t* mpctx)
{
	player_t::video_frame_t pending_frame{};
	bool has_pending = false;

	while (true)
	{
		auto state = mpctx->state.load();

		if (state == player_t::STOPPED)
			break;

		static bool once = true;
		if (state == player_t::PAUSED || state == player_t::SEEKING)
		{
			if (once) { printf("video_frame\n"); once = false; }
			sleep_for(10);
			continue;
		}
		once = true;


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
				auto wait_ms = std::min<uint64_t>((pending_frame.pts > now) ? (pending_frame.pts - now) : 0, 5);
				sleep_for(wait_ms);
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
				sleep_for(1);
			}
		}
	}
}

void audio_frame(player_t* mpctx)
{
	player_t::audio_frame_t pending_frame{};
	bool has_pending = false;

	while (true)
	{
		auto state = mpctx->state.load();

		if (state == player_t::STOPPED)
			break;

		static bool once = true;
		if (state == player_t::PAUSED || state == player_t::SEEKING)
		{
			if (once) { printf("audio_frame\n"); once = false; }
			sleep_for(10);
			continue;
		}
		once = true;


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
				auto wait_ms = std::min<uint64_t>((pending_frame.pts > now) ? (pending_frame.pts - now) : 0, 5);
				sleep_for(wait_ms);
				continue;
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
			else
			{
				sleep_for(1);
			}
		}
	}
}

void play_vframe(player_t* mpctx)
{
	while (true)
	{
		auto state = mpctx->state.load();

		if (state == player_t::STOPPED)
			break;

		static bool once = true;
		if (state == player_t::PAUSED || state == player_t::SEEKING)
		{
			if (once) { printf("player_video_frame\n"); once = false; }
			sleep_for(10);
			continue;
		}
		once = true;

		printf("pop_begin\n");
		player_t::video_frame_t frame{};
		if (!mpctx->play_vframes.pop(frame)) continue;
		printf("pop_end\n");

		SDL_UpdateYUVTexture(mpctx->texture, 0,
			frame.planes[0].data(), frame.strides[0],
			frame.planes[1].data(), frame.strides[1],
			frame.planes[2].data(), frame.strides[2]);

		SDL_RenderClear(mpctx->renderer);
		SDL_RenderTexture(mpctx->renderer, mpctx->texture, 0, 0);
		SDL_RenderPresent(mpctx->renderer);
	}
}

void play_aframe(player_t* mpctx)
{
	while (true)
	{
		auto state = mpctx->state.load();

		if (state == player_t::STOPPED)
			break;

		static bool once = true;
		if (state == player_t::PAUSED || state == player_t::SEEKING)
		{
			if (once) { printf("player_audio_frame\n"); once = false; }
			sleep_for(10);
			continue;
		}
		once = true;

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
			for (; i + 15 < count; i += 16)
			{
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

int wmain(int argc, wchar_t** argv)
{
	std::vector<char> password{ 'z', '1', 'x', '2', 'c', '3', 'v', '4', 'b', '5', 'n', '6' };
	std::wstring filepath = L"C:\\C\\1_x265_enc.mp4";

	std::unique_ptr<player_t> mpctx = std::make_unique<player_t>();
	player_t* ptrmpctx = mpctx.get();

	mpctx->stream = std::make_unique<memstream_t>(filepath, password);
	if (!mpctx->stream->is_valid()) return 4;

	mpctx->mp4 = std::make_unique<mp4_t>();
	if (!mpctx->mp4->parse(mpctx->stream.get())) return 5;

	for (const auto& track : mpctx->mp4->_tracks)
	{
		if (!mpctx->video_track && track.type == 'vide') mpctx->video_track = &track;
		if (!mpctx->audio_track && track.type == 'soun') mpctx->audio_track = &track;
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

	mpctx->state.store(player_t::PLAYING);
	mpctx->base_clock = std::chrono::steady_clock::now();

	std::jthread(decode_video, ptrmpctx).detach();
	std::jthread(decode_audio, ptrmpctx).detach();
	std::jthread(video_frame, ptrmpctx).detach();
	std::jthread(audio_frame, ptrmpctx).detach();
	std::jthread(play_vframe, ptrmpctx).detach();
	std::jthread(play_aframe, ptrmpctx).detach();

	button_t ck_quit('Q', 10);
	button_t ck_pause(VK_SPACE, 150);
	button_t ck_vup(VK_UP, 100);
	button_t ck_vdown(VK_DOWN, 100);
	button_t ck_lseek(VK_LEFT, 300);
	button_t ck_rseek(VK_RIGHT, 300);

	while (mpctx->state.load() != player_t::STOPPED)
	{
		if (ck_quit.is_pressed())
		{
			mpctx->state.store(player_t::STOPPED);
		}

		if (ck_pause.is_pressed())
		{
			auto state = mpctx->state.load();

			if (state == player_t::PLAYING)
			{
				mpctx->pause_time = std::chrono::steady_clock::now();
				mpctx->state.store(player_t::PAUSED);
			}
			else if (state == player_t::PAUSED)
			{
				auto resume_time = std::chrono::steady_clock::now();
				auto paused_duration = resume_time - mpctx->pause_time;
				mpctx->base_clock += paused_duration;
				mpctx->state.store(player_t::PLAYING);
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
			handle_seek(ptrmpctx, -1000);
		}

		if (ck_rseek.is_pressed())
		{
			handle_seek(ptrmpctx, +1000);
		}

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

		sleep_for(10);
	}

	SDL_Quit();

	return 0;
}