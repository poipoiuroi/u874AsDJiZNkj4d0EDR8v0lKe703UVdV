#include "include.h"

inline void sleep_for(uint64_t ms)
{
	std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

uint64_t get_playback_time(player_t* mpctx)
{
	auto now = std::chrono::steady_clock::now();
	auto delta = now - mpctx->base_clock;
	return static_cast<uint64_t>(std::chrono::duration<double, std::milli>(delta).count());
}

size_t find_keyframe_idx(const std::vector<sample_t>& samples, const stss_atom_t* stss, uint64_t target_ms, uint32_t timescale)
{
	if (samples.empty() || !stss || stss->entries.empty()) return 0;

	uint64_t target_pts = static_cast<uint64_t>((target_ms / 1000.0) * timescale);

	size_t sample_idx = 0;
	for (size_t i = samples.size(); i-- > 0;)
	{
		if (samples[i].presentation_time <= target_pts)
		{
			sample_idx = i;
			break;
		}
	}

	size_t key_idx = 0;
	for (size_t i = 0; i < stss->entries.size(); ++i)
	{
		uint32_t kf_sample_num = stss->entries[i] - 1;
		if (kf_sample_num <= sample_idx)
			key_idx = kf_sample_num;
		else break;
	}

	return key_idx;
}

void handle_seek(player_t* mpctx, int64_t delta_ms)
{
	if (!mpctx || !mpctx->video_track || !mpctx->audio_track) return;

	mpctx->state.store(player_t::SEEKING);

	int64_t current_time = static_cast<int64_t>(get_playback_time(mpctx));
	int64_t target_time = std::max<int64_t>(0, current_time + delta_ms);

	{
		std::lock_guard<std::mutex> lock(mpctx->stream_mutex);

		mpctx->video_frames.clear();
		mpctx->audio_frames.clear();
		mpctx->play_vframes.clear();
		mpctx->play_aframes.clear();

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

		//mpctx->v_idx.store(find_keyframe_idx(v_samples, mpctx->video_track->stss, target_time, mpctx->video_track->timescale));
		mpctx->v_idx.store(find_sample_idx(v_samples, target_time, mpctx->video_track->timescale));
		mpctx->a_idx.store(find_sample_idx(a_samples, target_time, mpctx->audio_track->timescale));

		mpctx->base_clock = std::chrono::steady_clock::now() - std::chrono::milliseconds(target_time);
	}

	std::this_thread::sleep_for(std::chrono::milliseconds(20));

	mpctx->state.store(player_t::PLAYING);
	mpctx->force_audio_frame_reset.store(true);
	mpctx->force_video_frame_reset.store(true);
}

void decode_video(player_t* mpctx)
{
	const auto& v_samples = mpctx->video_track->samples;

	const uint8_t start_code[4] = { 0x00, 0x00, 0x00, 0x01 };

	std::vector<uint8_t> hevc_init_nalus;
	for (const auto& arr : mpctx->video_track->stsd->nal_units)
		for (const auto& nal : arr)
		{
			hevc_init_nalus.insert(hevc_init_nalus.end(), start_code, start_code + 4);
			hevc_init_nalus.insert(hevc_init_nalus.end(), nal.begin(), nal.end());
		}

	de265_decoder_context* decoder = de265_new_decoder();
	if (!decoder)
		return;

	de265_push_data(decoder, hevc_init_nalus.data(), hevc_init_nalus.size(), 0, 0);
	de265_flush_data(decoder);

	while (true)
	{
		auto state = mpctx->state.load();
		if (state == player_t::STOPPED)
			break;

		if (state == player_t::PAUSED || state == player_t::SEEKING)
		{
			sleep_for(10);

			if (state == player_t::SEEKING)
			{
				de265_reset(decoder);
				de265_push_data(decoder, hevc_init_nalus.data(), hevc_init_nalus.size(), 0, 0);
				de265_flush_data(decoder);
			}

			continue;
		}

		size_t idx = mpctx->v_idx.load();
		if (idx >= v_samples.size())
		{
			sleep_for(10);
			continue;
		}

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
			err = de265_decode(decoder, &more);
			if (!de265_isOK(err))
				break;

			const de265_image* img = nullptr;
			while ((img = de265_get_next_picture(decoder)) != nullptr)
			{
				player_t::video_frame_t f{};
				f.pts = de265_get_image_PTS(img);
				f.width = de265_get_image_width(img, 0);
				f.height = de265_get_image_height(img, 0);

				for (int c = 0; c < 3; ++c)
				{
					int stride;
					const uint8_t* plane = de265_get_image_plane(img, c, &stride);
					int h = de265_get_image_height(img, c);
					f.planes[c].assign(plane, plane + stride * h);
					f.strides[c] = stride;
				}

				mpctx->video_frames.push(std::move(f));
			}

		} while (more);

		mpctx->v_idx.fetch_add(1);
	}

	de265_free_decoder(decoder);
}

void decode_audio(player_t* mpctx)
{
	const auto& a_samples = mpctx->audio_track->samples;

	while (true)
	{
		auto state = mpctx->state.load();
		if (state == player_t::STOPPED)
			break;

		if (state == player_t::PAUSED || state == player_t::SEEKING)
		{
			sleep_for(10);
			continue;
		}

		size_t idx = mpctx->a_idx.load();
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

		mpctx->dec_audiof.store(false);

		while (true)
		{
			state = mpctx->state.load();
			if (state == player_t::STOPPED || state == player_t::SEEKING)
				break;

			idx = mpctx->a_idx.load();
			if (idx >= a_samples.size())
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
				std::vector<int16_t> pcm(2048 * 2 * 2);
				if (aacDecoder_DecodeFrame(aac_decoder, pcm.data(), pcm.size(), 0) == AAC_DEC_OK)
				{
					const CStreamInfo* info = aacDecoder_GetStreamInfo(aac_decoder);
					if (info && info->sampleRate && info->numChannels)
					{
						pcm.resize(info->frameSize * info->numChannels);

						player_t::audio_frame_t s{};
						s.sample_rate = info->sampleRate;
						s.channels = info->numChannels;
						s.frame_size = info->frameSize;
						s.pts = sample.decode_time * 1000ull / mpctx->audio_track->timescale;
						s.pcm = std::move(pcm);

						state = mpctx->state.load();
						if (state != player_t::STOPPED && state != player_t::SEEKING)
						{
							mpctx->audio_frames.push(std::move(s));
						}
					}
				}
			}

			mpctx->a_idx.fetch_add(1);
		}

		aacDecoder_Close(aac_decoder);
		mpctx->dec_audiof.store(true);

		state = mpctx->state.load();
		if (state != player_t::SEEKING) mpctx->state.store(player_t::STOPPED);
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

		if (state == player_t::PAUSED || state == player_t::SEEKING)
		{
			has_pending = false;
			memset(&pending_frame, 0, sizeof(pending_frame));
			sleep_for(10);
			continue;
		}

		if (mpctx->force_video_frame_reset.load())
		{
			mpctx->video_frames.pop_front();
			has_pending = false;
			memset(&pending_frame, 0, sizeof(pending_frame));
			mpctx->force_video_frame_reset.store(false);
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
				sleep_for(std::min<uint64_t>(pending_frame.pts - now, 5));
				continue;
			}
		}
		else
		{
			player_t::video_frame_t frame{};
			if (mpctx->video_frames.try_pop(frame))
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

		if (state == player_t::PAUSED || state == player_t::SEEKING)
		{
			has_pending = false;
			memset(&pending_frame, 0, sizeof(pending_frame));
			sleep_for(10);
			continue;
		}

		if (mpctx->force_audio_frame_reset.load())
		{
			mpctx->audio_frames.pop_front();
			has_pending = false;
			memset(&pending_frame, 0, sizeof(pending_frame));
			mpctx->force_audio_frame_reset.store(false);
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
				sleep_for(std::min<uint64_t>(pending_frame.pts - now, 5));
				continue;
			}
		}
		else
		{
			player_t::audio_frame_t frame{};
			if (mpctx->audio_frames.try_pop(frame))
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

		if (state == player_t::PAUSED || state == player_t::SEEKING)
		{
			sleep_for(10);
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
	}
}

void play_aframe(player_t* mpctx)
{
	while (true)
	{
		auto state = mpctx->state.load();
		if (state == player_t::STOPPED)
			break;

		if (state == player_t::PAUSED || state == player_t::SEEKING)
		{
			sleep_for(10);
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

	bool is_audio = (mpctx->audio_track && !mpctx->audio_track->stsd->asc_bytes.empty());
	bool is_video = (mpctx->video_track && !mpctx->video_track->stsd->nal_units.empty());

	mpctx->state.store(player_t::PLAYING);
	mpctx->base_clock = std::chrono::steady_clock::now();

	if (is_audio)
	{
		std::jthread(decode_audio, ptrmpctx).detach();
		std::jthread(audio_frame, ptrmpctx).detach();
		std::jthread(play_aframe, ptrmpctx).detach();
	}

	if (is_video)
	{
		std::jthread(decode_video, ptrmpctx).detach();
		std::jthread(video_frame, ptrmpctx).detach();
		std::jthread(play_vframe, ptrmpctx).detach();
	}

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

	button_t ck_quit('Q', 10);
	button_t ck_pause(VK_SPACE, 150);
	button_t ck_vup(VK_UP, 100);
	button_t ck_vdown(VK_DOWN, 100);
	button_t ck_lseek(VK_LEFT, 100);
	button_t ck_rseek(VK_RIGHT, 100);

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

		sleep_for(20);
	}

	return 0;
}