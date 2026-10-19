// nn::codec's hardware Opus decoder at the API level, with libopus on the host (the hwopus service is libopus on
// the console's audio DSP). Each input frame carries Nintendo's 8-byte big-endian header {payload size, final range};
// the decoder's final range is checked against it. The decoder state stays on the host: the game's work buffer is
// reserved but unused. Without this the sdk's client talked to the stub hwopus service, got no decoder and crashed
// in DecodeOpusInterleavedWithHardware (Tears of the Kingdom). As MacNativeGames' opus_codec.c.
// Built where libopus is linked (NX_HAS_OPUS: the PS4 build, PacBrew's libopus).
#if defined(NX_HAS_OPUS)

#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>

#include <opus/opus.h>

#include <nx/runtime.h>

namespace nx {

namespace {

constexpr uint32_t kInvalid = 0x1FCD1;  // codec module 111: invalid argument

struct Decoder {
    OpusDecoder* dec = nullptr;
    int rate = 0, channels = 0;
};
std::mutex g_lock;
std::map<uint64_t, Decoder> g_decoders;  // by the game's HardwareOpusDecoderType*

bool valid_format(int rate, int channels) {
    return (rate == 48000 || rate == 24000 || rate == 16000 || rate == 12000 || rate == 8000) &&
           (channels == 1 || channels == 2);
}

uint64_t work_size(int rate, int channels) {
    return valid_format(rate, channels) ? uint64_t(opus_decoder_get_size(channels)) + 0x1000 : 0;
}

Decoder* find(uint64_t guest) {
    std::lock_guard lk(g_lock);
    auto it = g_decoders.find(guest);
    return it == g_decoders.end() ? nullptr : &it->second;
}

void GetWorkBufferSize(CPU& c) { c.x[0] = work_size(int(c.x[0]), int(c.x[1])); }  // (int rate, int channels, int)

// (HardwareOpusDecoderType*, int rate, int channels, void* work, size_t size, int option)
void Initialize(CPU& c) {
    uint64_t guest = c.x[0];
    int rate = int(c.x[1]), channels = int(c.x[2]);
    if (!guest || !valid_format(rate, channels) || c.x[4] < work_size(rate, channels)) {
        c.x[0] = kInvalid;
        return;
    }
    int err = 0;
    OpusDecoder* dec = opus_decoder_create(rate, channels, &err);
    if (!dec || err != OPUS_OK) fatal("opus_decoder_create(%d, %d): %s", rate, channels, opus_strerror(err));
    std::lock_guard lk(g_lock);
    Decoder& d = g_decoders[guest];
    if (d.dec) opus_decoder_destroy(d.dec);
    d = {dec, rate, channels};
    static bool logged = false;
    if (!logged) log("codec: hardware Opus decoder on libopus %s", opus_get_version_string()), logged = true;
    c.x[0] = 0;
}

void Finalize(CPU& c) {
    std::lock_guard lk(g_lock);
    auto it = g_decoders.find(c.x[0]);
    if (it != g_decoders.end()) {
        opus_decoder_destroy(it->second.dec);
        g_decoders.erase(it);
    }
}

void GetSampleRate(CPU& c) {
    Decoder* d = find(c.x[0]);
    c.x[0] = d ? uint64_t(d->rate) : 0;
}

void GetChannelCount(CPU& c) {
    Decoder* d = find(c.x[0]);
    c.x[0] = d ? uint64_t(d->channels) : 0;
}

void Reset(CPU& c) {
    Decoder* d = find(c.x[0]);
    c.x[0] = d && opus_decoder_ctl(d->dec, OPUS_RESET_STATE) == OPUS_OK ? 0 : kInvalid;
}

uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

// (HardwareOpusDecoderType*, size_t* consumed, int* samples, int16_t* out, size_t out_size, const void* in,
//  size_t in_size)
void Decode(CPU& c) {
    Decoder* d = find(c.x[0]);
    uint64_t consumed = c.x[1], samples = c.x[2], out = c.x[3], out_size = c.x[4], in = c.x[5], in_size = c.x[6];
    if (!d || !consumed || !samples || !out || !in || in_size < 8) {
        c.x[0] = kInvalid;
        return;
    }
    const uint8_t* p = reinterpret_cast<const uint8_t*>(in);
    uint32_t n = be32(p), range = be32(p + 4);
    if (n > in_size - 8) {
        c.x[0] = kInvalid;
        return;
    }
    int max = int(out_size / (sizeof(int16_t) * uint64_t(d->channels)));
    int got = opus_decode(d->dec, p + 8, opus_int32(n), reinterpret_cast<opus_int16*>(out), max, 0);
    if (got < 0) {
        static int logged = 0;
        if (logged++ < 10) log("codec: opus_decode: %s", opus_strerror(got));
        c.x[0] = kInvalid;
        return;
    }
    opus_uint32 final_range = 0;
    opus_decoder_ctl(d->dec, OPUS_GET_FINAL_RANGE(&final_range));
    if (n && final_range != range) {
        static int logged = 0;
        if (logged++ < 10) log("codec: Opus final range %#x, the stream says %#x", final_range, range);
    }
    st<uint64_t>(consumed, 8 + n);
    st<int32_t>(samples, got);
    c.x[0] = 0;
}

}  // namespace

NX_HLE("_ZN2nn5codec36GetHardwareOpusDecoderWorkBufferSizeEiii", GetWorkBufferSize);
NX_HLE("_ZN2nn5codec29InitializeHardwareOpusDecoderEPNS0_23HardwareOpusDecoderTypeEiiPvmi", Initialize);
NX_HLE("_ZN2nn5codec27FinalizeHardwareOpusDecoderEPNS0_23HardwareOpusDecoderTypeE", Finalize);
NX_HLE("_ZN2nn5codec32GetHardwareOpusDecoderSampleRateEPKNS0_23HardwareOpusDecoderTypeE", GetSampleRate);
NX_HLE("_ZN2nn5codec34GetHardwareOpusDecoderChannelCountEPKNS0_23HardwareOpusDecoderTypeE", GetChannelCount);
NX_HLE("_ZN2nn5codec24ResetHardwareOpusDecoderEPNS0_23HardwareOpusDecoderTypeE", Reset);
NX_HLE("_ZN2nn5codec33DecodeOpusInterleavedWithHardwareEPNS0_23HardwareOpusDecoderTypeEPmPiPsmPKvm", Decode);

}  // namespace nx

#endif  // NX_HAS_OPUS
