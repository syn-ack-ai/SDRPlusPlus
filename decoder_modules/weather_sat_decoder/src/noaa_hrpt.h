#pragma once
#include <cstdint>
#include <cstring>
#include <algorithm>

namespace noaa {
    // NOAA HRPT protocol constants
    constexpr int HRPT_SYMBOL_RATE = 665400;
    constexpr int HRPT_VFO_SR = 3000000;
    constexpr int HRPT_VFO_BW = 2000000;
    constexpr int HRPT_WORDS_PER_FRAME = 11090;
    constexpr int HRPT_BITS_PER_WORD = 10;
    constexpr int HRPT_FRAME_BITS = HRPT_WORDS_PER_FRAME * HRPT_BITS_PER_WORD; // 110900
    constexpr int HRPT_MANCH_FRAME_BITS = HRPT_FRAME_BITS * 2;                 // 221800
    constexpr int HRPT_SYNC_WORDS = 6;
    constexpr int HRPT_SYNC_BITS = HRPT_SYNC_WORDS * HRPT_BITS_PER_WORD;       // 60
    constexpr int HRPT_MANCH_SYNC_BITS = HRPT_SYNC_BITS * 2;                   // 120
    constexpr int HRPT_AVHRR_PIXELS = 2048;
    constexpr int HRPT_AVHRR_CHANNELS = 5;
    constexpr int HRPT_AVHRR_START_WORD = 750;

    // HRPT frame sync pattern (decoded, 60 bits packed into 8 bytes MSB first).
    // Standard NOAA HRPT sync: first 6 ten-bit words of each minor frame.
    // Source: NOAA Polar Orbiter Data User's Guide.
    // NOTE: If images appear garbled, this sync word may need verification
    // against actual satellite data.
    static const uint8_t HRPT_SYNC_PACKED[] = { 0x02, 0x84, 0x01, 0x68, 0x80, 0xA2, 0x00, 0x00 };

    // Unpack the decoded sync word to individual bits (60 bits)
    inline void getDecodedSyncBits(uint8_t* out) {
        for (int i = 0; i < HRPT_SYNC_BITS; i++) {
            int byteIdx = i / 8;
            int bitIdx = 7 - (i % 8);
            out[i] = (HRPT_SYNC_PACKED[byteIdx] >> bitIdx) & 1;
        }
    }

    // Generate Manchester-encoded sync word (120 bits) from decoded sync (60 bits).
    // Manchester convention: bit 1 -> [1,0], bit 0 -> [0,1]
    inline void getManchesterSyncBits(uint8_t* out) {
        uint8_t decoded[HRPT_SYNC_BITS];
        getDecodedSyncBits(decoded);
        for (int i = 0; i < HRPT_SYNC_BITS; i++) {
            out[2 * i]     = decoded[i];
            out[2 * i + 1] = 1 - decoded[i];
        }
    }

    // Correlate a bit window against the Manchester sync word.
    // Returns number of matching bits (out of HRPT_MANCH_SYNC_BITS=120).
    inline int correlateSync(const uint8_t* bits, const uint8_t* manchSync) {
        int matches = 0;
        for (int i = 0; i < HRPT_MANCH_SYNC_BITS; i++) {
            if (bits[i] == manchSync[i]) { matches++; }
        }
        return matches;
    }

    // Manchester decode: take every other bit (first of each pair).
    // Input: manchBits (count bits), Output: decoded (count/2 bits).
    inline int manchesterDecode(const uint8_t* manchBits, uint8_t* decoded, int count) {
        int outCount = count / 2;
        for (int i = 0; i < outCount; i++) {
            decoded[i] = manchBits[2 * i];
        }
        return outCount;
    }

    // Extract a 10-bit word from a bit array at the given bit offset (MSB first)
    inline uint16_t extractWord(const uint8_t* bits, int bitOffset) {
        uint16_t val = 0;
        for (int i = 0; i < HRPT_BITS_PER_WORD; i++) {
            val = (val << 1) | bits[bitOffset + i];
        }
        return val;
    }

    // Extract AVHRR channel data from a decoded HRPT minor frame.
    // Input: frameBits (HRPT_FRAME_BITS decoded bits, one bit per byte)
    // Output: channels[channel][pixel], 5 channels of 2048 10-bit pixels
    // AVHRR data is interleaved starting at word HRPT_AVHRR_START_WORD:
    //   pixel P, channel C -> word (HRPT_AVHRR_START_WORD + P * 5 + C)
    inline void extractAVHRR(const uint8_t* frameBits,
                             uint16_t channels[HRPT_AVHRR_CHANNELS][HRPT_AVHRR_PIXELS]) {
        for (int px = 0; px < HRPT_AVHRR_PIXELS; px++) {
            for (int ch = 0; ch < HRPT_AVHRR_CHANNELS; ch++) {
                int wordIdx = HRPT_AVHRR_START_WORD + (px * HRPT_AVHRR_CHANNELS) + ch;
                if (wordIdx >= HRPT_WORDS_PER_FRAME) {
                    channels[ch][px] = 0;
                    continue;
                }
                channels[ch][px] = extractWord(frameBits, wordIdx * HRPT_BITS_PER_WORD);
            }
        }
    }
}
