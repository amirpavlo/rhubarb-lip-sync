#include <format.h>
#include "WaveFileReader.h"
#include "io_tools.h"

using std::runtime_error;
using fmt::format;
using std::string;
using namespace little_endian;

#define INT24_MIN (-8388608)
#define INT24_MAX 8388607

// Converts an int in the range min..max to a float in the range -1..1
float toNormalizedFloat(int value, int min, int max) {
	return (static_cast<float>(value - min) / (max - min) * 2) - 1;
}

int roundToEven(int i) {
	return (i + 1) & (~1);
}

enum class Codec {
	PCM = 0x01,
	Float = 0x03
};

WaveFileReader::WaveFileReader(std::string fileName) {
	// Open file
	file.exceptions(std::ifstream::failbit | std::ifstream::badbit);
	file.open(fileName, std::ios::binary);

	// Read header
	uint32_t rootChunkId = read<uint32_t>(file);
	if (rootChunkId != fourcc('R', 'I', 'F', 'F')) {
		throw runtime_error("Unknown file format. Only WAVE files are supported.");
	}
	read<uint32_t>(file); // Chunk size
	uint32_t waveId = read<uint32_t>(file);
	if (waveId != fourcc('W', 'A', 'V', 'E')) {
		throw runtime_error(format("File format is not WAVE, but {}.", fourccToString(waveId)));
	}

	// Read chunks until we reach the data chunk
	bool reachedDataChunk = false;
	int bytesPerSample = 0;
	do {
		uint32_t chunkId = read<uint32_t>(file);
		int chunkSize = read<uint32_t>(file);
		switch (chunkId) {
			case fourcc('f', 'm', 't', ' '): {
				// Read relevant data
				Codec codec = (Codec) read<uint16_t>(file);
				channelCount = read<uint16_t>(file);
				frameRate = read<uint32_t>(file);
				read<uint32_t>(file); // Bytes per second
				int frameSize = read<uint16_t>(file);
				int bitsPerSample = read<uint16_t>(file);

				// We're read 16 bytes so far. Skip the remainder.
				file.seekg(roundToEven(chunkSize) - 16, file.cur);

				// Determine sample format
				switch (codec) {
					case Codec::PCM:
						// Determine sample size.
						// According to the WAVE standard, sample sizes that are not multiples of 8 bits
						// (e.g. 12 bits) can be treated like the next-larger byte size.
						if (bitsPerSample == 8) {
							sampleFormat = SampleFormat::UInt8;
							bytesPerSample = 1;
						} else if (bitsPerSample <= 16) {
							sampleFormat = SampleFormat::Int16;
							bytesPerSample = 2;
						} else if (bitsPerSample <= 24) {
							sampleFormat = SampleFormat::Int24;
							bytesPerSample = 3;
						} else {
							throw runtime_error(
								format("Unsupported sample format: {}-bit integer samples.", bitsPerSample));
						}
						if (bytesPerSample != frameSize / channelCount) {
							throw runtime_error("Unsupported sample organization.");
						}
						break;
					case Codec::Float:
						if (bitsPerSample == 32) {
							sampleFormat = SampleFormat::Float32;
							bytesPerSample = 4;
						} else {
							throw runtime_error(format("Unsupported sample format: {}-bit floating-point samples.", bitsPerSample));
						}
						break;
					default:
						throw runtime_error("Unsupported sample format. Only uncompressed formats are supported.");
				}
				break;
			}
			case fourcc('d', 'a', 't', 'a'): {
				reachedDataChunk = true;
				remainingSamples = chunkSize / bytesPerSample;
				frameCount = remainingSamples / channelCount;
				break;
			}
			default: {
				// Skip unknown chunk
				file.seekg(roundToEven(chunkSize), file.cur);
				break;
			}
		}
	} while (!reachedDataChunk);
}

int WaveFileReader::getFrameRate() {
	return frameRate;
}

int WaveFileReader::getFrameCount() {
	return frameCount;
}

int WaveFileReader::getChannelCount() {
	return channelCount;
}

bool WaveFileReader::getNextSample(float &sample) {
	if (remainingSamples == 0) return false;
	remainingSamples--;

	switch (sampleFormat) {
		case SampleFormat::UInt8: {
			uint8_t raw = read<uint8_t>(file);
			sample = toNormalizedFloat(raw, 0, UINT8_MAX);
			break;
		}
		case SampleFormat::Int16: {
			int16_t raw = read<int16_t>(file);
			sample = toNormalizedFloat(raw, INT16_MIN, INT16_MAX);
			break;
		}
		case SampleFormat::Int24: {
			int raw = read<int, 24>(file);
			if (raw & 0x800000) raw |= 0xFF000000; // Fix two's complement
			sample = toNormalizedFloat(raw, INT24_MIN, INT24_MAX);
			break;
		}
		case SampleFormat::Float32: {
			sample = read<float>(file);
			break;
		}
	}
	return true;
}
