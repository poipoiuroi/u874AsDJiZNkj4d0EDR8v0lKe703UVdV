#include "include.h"
#include <algorithm>
#include <functional>

void mdhd_atom_t::parse(memstream_t* stream)
{
	stream->seekg(_offset + 12);
	stream->ignore(8);
	stream->read(&timescale, 4);
	stream->read(&duration, 4);
	timescale = bswap32(timescale);
	duration = bswap32(duration);
}

void hdlr_atom_t::parse(memstream_t* stream)
{
	stream->seekg(_offset + 12);
	stream->ignore(4);
	stream->read(&type, 4);
	type = bswap32(type);
}

void stsz_atom_t::parse(memstream_t* stream)
{
	stream->seekg(_offset + 12);

	uint32_t sample_size, sample_count;
	stream->read(&sample_size, 4);
	stream->read(&sample_count, 4);
	sample_size = bswap32(sample_size);
	sample_count = bswap32(sample_count);

	if (sample_size == 0)
	{
		for (uint32_t i = 0; i < sample_count; ++i)
		{
			uint32_t entry_size;
			stream->read(&entry_size, 4);
			entry_size = bswap32(entry_size);
			entries.push_back(entry_size);
		}
	}
}

void stsc_atom_t::parse(memstream_t* stream)
{
	stream->seekg(_offset + 12);

	uint32_t entry_count = 0;
	stream->read(&entry_count, 4);
	entry_count = bswap32(entry_count);

	for (uint32_t i = 0; i < entry_count; ++i)
	{
		entry_t e{};
		stream->read(&e.first_chunk, 4);
		stream->read(&e.samples_per_chunk, 4);
		stream->read(&e.sample_description_index, 4);
		e.first_chunk = bswap32(e.first_chunk);
		e.samples_per_chunk = bswap32(e.samples_per_chunk);
		e.sample_description_index = bswap32(e.sample_description_index);
		entries.push_back(e);
	}
}

void stco_atom_t::parse(memstream_t* stream)
{
	stream->seekg(_offset + 12);

	uint32_t entry_count = 0;
	stream->read(&entry_count, 4);
	entry_count = bswap32(entry_count);

	for (uint32_t i = 0; i < entry_count; ++i)
	{
		uint32_t chunk_offset;
		stream->read(&chunk_offset, 4);
		chunk_offset = bswap32(chunk_offset);
		entries.push_back(chunk_offset);
	}
}

void stts_atom_t::parse(memstream_t* stream)
{
	stream->seekg(_offset + 12);

	uint32_t entry_count;
	stream->read(&entry_count, 4);
	entry_count = bswap32(entry_count);

	for (uint32_t i = 0; i < entry_count; ++i)
	{
		entry_t e{};
		stream->read(&e.sample_count, 4);
		stream->read(&e.sample_delta, 4);
		e.sample_count = bswap32(e.sample_count);
		e.sample_delta = bswap32(e.sample_delta);
		entries.push_back(e);
	}
}

void ctts_atom_t::parse(memstream_t* stream)
{
	stream->seekg(_offset + 12);

	uint32_t entry_count = 0;
	stream->read(&entry_count, 4);
	entry_count = bswap32(entry_count);

	for (uint32_t i = 0; i < entry_count; ++i)
	{
		entry_t e{};
		stream->read(&e.sample_count, 4);
		stream->read(&e.sample_offset, 4);
		e.sample_count = bswap32(e.sample_count);
		e.sample_offset = bswap32(e.sample_offset);
		entries.push_back(e);
	}
}

void stss_atom_t::parse(memstream_t* stream)
{
	stream->seekg(_offset + 12);

	uint32_t entry_count = 0;
	stream->read(&entry_count, 4);
	entry_count = bswap32(entry_count);

	entries.reserve(entry_count);
	for (uint32_t i = 0; i < entry_count; ++i)
	{
		uint32_t sample_number;
		stream->read(&sample_number, 4);
		sample_number = bswap32(sample_number);
		entries.push_back(sample_number);
	}
}

void stsd_atom_t::parse(memstream_t* stream, uint32_t htype)
{
	stream->seekg(_offset + 12);

	uint32_t entry_count;
	stream->read(&entry_count, 4);
	entry_count = bswap32(entry_count);

	uint32_t atom_name = 0;

	if (htype == 'vide')
	{
		for (uint32_t i = 0; i < entry_count; ++i)
		{
			stream->ignore(4);

			stream->read(&atom_name, 4);
			atom_name = bswap32(atom_name);

			if (atom_name == 'hev1')
			{
				stream->ignore(24);

				stream->read(&width, 2);
				width = bswap16(width);

				stream->read(&height, 2);
				height = bswap16(height);

				stream->ignore(54);

				stream->read(&atom_name, 4);
				atom_name = bswap32(atom_name);

				if (atom_name == 'hvcC')
				{
					stream->ignore(21);

					uint8_t fr;
					stream->read(&fr, 1);

					uint8_t num_of_arrays;
					stream->read(&num_of_arrays, 1);

					for (int q = 0; q < num_of_arrays; q++)
					{
						stream->ignore(1);

						uint16_t num_nalus;
						stream->read(&num_nalus, 2);
						num_nalus = bswap16(num_nalus);

						std::vector<std::vector<uint8_t>> tmp;
						for (int w = 0; w < num_nalus; w++)
						{
							uint16_t nal_size;
							stream->read(&nal_size, 2);
							nal_size = bswap16(nal_size);

							std::vector<uint8_t> nalu(nal_size);
							stream->read(nalu.data(), nal_size);
							tmp.push_back(std::move(nalu));
						}

						nal_units.push_back(std::move(tmp));
					}
				}
			}
		}
	}

	if (htype == 'soun')
	{
		for (uint32_t i = 0; i < entry_count; ++i)
		{
			uint32_t mp4a_size;
			stream->read(&mp4a_size, 4);
			mp4a_size = bswap32(mp4a_size);

			stream->read(&atom_name, 4);
			atom_name = bswap32(atom_name);

			if (atom_name == 'mp4a')
			{
				stream->ignore(16);

				stream->read(&channel_count, 2);
				channel_count = bswap16(channel_count);

				stream->read(&sample_size, 2);
				sample_size = bswap16(sample_size);

				stream->ignore(4);

				uint32_t sample_rate_fixed;
				stream->read(&sample_rate_fixed, 4);
				sample_rate_fixed = bswap32(sample_rate_fixed);
				sample_rate = sample_rate_fixed / 65536.0f;

				stream->ignore(4);

				stream->read(&atom_name, 4);
				atom_name = bswap32(atom_name);

				if (atom_name == 'esds')
				{
					stream->ignore(4);

					uint8_t tag;
					stream->read(&tag, 1);
					if (tag != 0x03) return;

					uint8_t b;
					do { stream->read(&b, 1); } while (b == 0x80);
					stream->ignore(3);

					stream->read(&tag, 1);
					do { stream->read(&b, 1); } while (b == 0x80);
					stream->ignore(13);

					stream->read(&tag, 1);
					do { stream->read(&b, 1); } while (b == 0x80);

					uint8_t asc_size = b;
					asc_bytes.resize(asc_size);
					stream->read(asc_bytes.data(), asc_size);
				}
			}
		}
	}
}

void atom_t::parse(memstream_t* stream)
{
	uint64_t offset = _offset + 8;
	uint64_t max_offset = _offset + _size;

	while (offset + 8 <= max_offset)
	{
		stream->seekg(offset);

		uint32_t size32;
		char type_buf[4];
		stream->read(&size32, 4);
		stream->read(type_buf, 4);

		if (!stream || stream->gcount() < 4 || !is_valid_atom_type(type_buf)) break;

		uint64_t child_size = bswap32(size32);
		std::string child_type(type_buf, 4);

		offset += 8;

		if (child_size < 8 || offset + child_size - 8 > max_offset) break;

		auto child = create_atom(offset - 8, child_size, child_type);
		child->parse(stream);
		_children.push_back(std::move(child));
		offset += child_size - 8;
	}
}

bool mp4_t::parse(memstream_t* stream)
{
	size_t max_offset = stream->size();

	uint64_t offset = 0;
	while (offset + 8 <= max_offset)
	{
		stream->seekg(offset);

		uint32_t size32;
		char type_buf[4]{};
		stream->read(&size32, 4);
		stream->read(type_buf, 4);

		if (!stream || stream->gcount() < 4 || !is_valid_atom_type(type_buf)) break;

		uint64_t size = bswap32(size32);
		std::string type(type_buf, 4);

		offset += 8;

		if (size < 8 || offset + size - 8 > max_offset) break;

		auto atom = atom_t::create_atom(offset - 8, size, type);
		atom->parse(stream);
		_atoms.push_back(std::move(atom));
		offset += size - 8;
	}

	std::function<void(const std::unique_ptr<atom_t>&, track_t& track)>
		visit = [&](const std::unique_ptr<atom_t>& node, track_t& track)
		{
			const std::string& t = node->_type;
			if (t == "hdlr") track.type = static_cast<hdlr_atom_t*>(node.get())->type;
			if (t == "stts") track.stts = static_cast<stts_atom_t*>(node.get());
			if (t == "ctts") track.ctts = static_cast<ctts_atom_t*>(node.get());
			if (t == "stsc") track.stsc = static_cast<stsc_atom_t*>(node.get());
			if (t == "stsz") track.stsz = static_cast<stsz_atom_t*>(node.get());
			if (t == "stco") track.stco = static_cast<stco_atom_t*>(node.get());
			if (t == "stss") track.stss = static_cast<stss_atom_t*>(node.get());
			if (t == "mdhd")
			{
				const auto& it = static_cast<mdhd_atom_t*>(node.get());

				track.duration = it->duration;
				track.timescale = it->timescale;
			}
			if (t == "stsd")
			{
				const auto& it = static_cast<stsd_atom_t*>(node.get());
				it->parse(stream, track.type);
				track.stsd = it;

				if (track.type == 'vide')
				{
					track.width = it->width;
					track.height = it->height;
				}

				if (track.type == 'soun')
				{
					track.channel_count = it->channel_count;
					track.sample_rate = it->sample_rate;
					track.sample_size = it->sample_size;
				}
			}

			for (const auto& c : node->_children) visit(c, track);
		};

	std::function<void(const std::unique_ptr<atom_t>&)>
		visit_trak = [&](const std::unique_ptr<atom_t>& atom)
		{
			if (atom->_type == "trak")
			{
				track_t track{};
				visit(atom, track);

				if ((track.type == 'vide' || track.type == 'soun') &&
					track.stts && track.stsc && track.stsz && track.stco && track.stsd)
				{
					build_samples(track);
					_tracks.push_back(std::move(track));
				}
			}

			for (const auto& child : atom->_children)
				visit_trak(child);
		};

	for (const auto& atom : _atoms)
		visit_trak(atom);

	return !_tracks.empty();
}

void mp4_t::build_samples(track_t& track)
{
	auto& stsz = *track.stsz;
	auto& stsc = *track.stsc;
	auto& stco = *track.stco;
	auto& stts = *track.stts;
	auto& ctts = *track.ctts;
	auto& stss = *track.stss;

	uint32_t sample_index = 0;
	uint32_t decode_time = 0;
	uint32_t stts_index = 0, stts_sample_pos = 0;
	uint32_t ctts_index = 0, ctts_sample_pos = 0;
	uint32_t sample_id = 1;

	for (size_t i = 0; i < stsc.entries.size(); ++i)
	{
		auto& entry = stsc.entries[i];
		uint32_t next_first_chunk = (i + 1 < stsc.entries.size()) ? stsc.entries[i + 1].first_chunk : static_cast<uint32_t>(stco.entries.size()) + 1;

		for (uint32_t chunk_id = entry.first_chunk; chunk_id < next_first_chunk; ++chunk_id)
		{
			uint32_t chunk_offset = stco.entries[chunk_id - 1];
			uint32_t samples_per_chunk = entry.samples_per_chunk;

			uint64_t offset = chunk_offset;

			for (uint32_t s = 0; s < samples_per_chunk && sample_index < stsz.entries.size(); ++s, ++sample_index)
			{
				auto size = stsz.entries[sample_index];

				uint32_t composition_offset = 0;
				if (track.ctts && ctts_index < ctts.entries.size())
				{
					auto& ctts_entry = ctts.entries[ctts_index];
					composition_offset = ctts_entry.sample_offset;
					if (++ctts_sample_pos >= ctts_entry.sample_count)
					{
						ctts_sample_pos = 0;
						++ctts_index;
					}
				}

				uint32_t duration = 0;
				if (track.stts && stts_index < stts.entries.size())
				{
					auto& stts_entry = stts.entries[stts_index];
					duration = stts_entry.sample_delta;

					if (++stts_sample_pos >= stts_entry.sample_count)
					{
						stts_sample_pos = 0;
						++stts_index;
					}
				}

				bool is_key = !track.stss ||
					std::binary_search(track.stss->entries.begin(), track.stss->entries.end(), sample_id);

				sample_t sample{};
				sample.file_offset = offset;
				sample.size = size;
				sample.decode_time = decode_time;
				sample.duration = duration;
				sample.composition_offset = composition_offset;
				sample.is_keyframe = is_key;
				sample.presentation_time = decode_time + composition_offset;

				track.samples.push_back(sample);

				offset += size;
				decode_time += duration;
				sample_id++;
			}
		}
	}
}

inline std::unique_ptr<atom_t> atom_t::create_atom(uint64_t offset, uint64_t size, const std::string& type)
{
	if (type == "mdhd") return std::make_unique<mdhd_atom_t>(offset, size, type);
	if (type == "stsd") return std::make_unique<stsd_atom_t>(offset, size, type);
	if (type == "hdlr") return std::make_unique<hdlr_atom_t>(offset, size, type);
	if (type == "stts") return std::make_unique<stts_atom_t>(offset, size, type);
	if (type == "stss") return std::make_unique<stss_atom_t>(offset, size, type);
	if (type == "ctts") return std::make_unique<ctts_atom_t>(offset, size, type);
	if (type == "stsc") return std::make_unique<stsc_atom_t>(offset, size, type);
	if (type == "stsz") return std::make_unique<stsz_atom_t>(offset, size, type);
	if (type == "stco") return std::make_unique<stco_atom_t>(offset, size, type);
	return std::make_unique<atom_t>(offset, size, type);
}