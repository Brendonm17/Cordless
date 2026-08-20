#include "media/mp4.h"

#include <stdlib.h>
#include <string.h>

static unsigned be32(const unsigned char *p) { return (p[0]<<24)|(p[1]<<16)|(p[2]<<8)|p[3]; }
static unsigned be16(const unsigned char *p) { return (p[0]<<8)|p[1]; }
static unsigned long long be64(const unsigned char *p)
{ return ((unsigned long long)be32(p) << 32) | be32(p + 4); }

// Iterate boxes in [d, d+len). Advances *p; returns 1 with the box's type ptr,
// contents ptr, and content length, or 0 at end / on malformed size.
static int next_box(const unsigned char *d, int len, int *p, const unsigned char **type,
                    const unsigned char **cont, int *clen)
{
    if (*p + 8 > len) return 0;
    unsigned sz = be32(d + *p);
    int hdr = 8; long long bsz = sz;
    if (sz == 1) { if (*p + 16 > len) return 0; bsz = (long long)be64(d + *p + 8); hdr = 16; }
    else if (sz == 0) bsz = len - *p;
    if (bsz < hdr || (long long)*p + bsz > len) return 0;
    *type = d + *p + 4; *cont = d + *p + hdr; *clen = (int)(bsz - hdr);
    *p += (int)bsz;
    return 1;
}

// First child box of `type` within [d, d+len). Returns contents + sets *clen.
static const unsigned char *find_box(const unsigned char *d, int len, const char *type, int *clen)
{
    int p = 0; const unsigned char *t, *c;
    while (next_box(d, len, &p, &t, &c, clen))
        if (memcmp(t, type, 4) == 0) return c;
    return NULL;
}

// Reconstruct a track's sample table (offset/size/keyframe/duration) from a
// stbl. Returns count, fills *out (caller frees), or 0 on error.
static int build_samples(const unsigned char *stbl, int stbl_len, mp4_sample **out)
{
    int stszl=0, stscl=0, stcol=0, co64l=0, sttsl=0, stssl=0;
    const unsigned char *stsz = find_box(stbl, stbl_len, "stsz", &stszl);
    const unsigned char *stsc = find_box(stbl, stbl_len, "stsc", &stscl);
    const unsigned char *stco = find_box(stbl, stbl_len, "stco", &stcol);
    const unsigned char *co64 = find_box(stbl, stbl_len, "co64", &co64l);
    const unsigned char *stts = find_box(stbl, stbl_len, "stts", &sttsl);
    const unsigned char *stss = find_box(stbl, stbl_len, "stss", &stssl);
    if (!stsz || stszl < 12 || !stsc || stscl < 8 || (!stco && !co64)) return 0;
    unsigned uniform = be32(stsz + 4);
    int nsamp = (int)be32(stsz + 8);
    if (nsamp <= 0 || nsamp > 500000) return 0;
    int nchunks = stco ? (int)be32(stco + 4) : (int)be32(co64 + 4);
    if (nchunks <= 0) return 0;
    mp4_sample *S = calloc((size_t)nsamp, sizeof(mp4_sample));
    if (!S) return 0;
    for (int i = 0; i < nsamp; i++) {
        if (uniform) S[i].size = uniform;
        else { int o = 12 + i * 4; S[i].size = (o + 4 <= stszl) ? be32(stsz + o) : 0; }
    }
    int nentries = (int)be32(stsc + 4), s = 0;
    for (int e = 0; e < nentries; e++) {
        int eo = 8 + e * 12; if (eo + 12 > stscl) break;
        int first_chunk = (int)be32(stsc + eo), spc = (int)be32(stsc + eo + 4);
        int last_chunk = (e + 1 < nentries && 8 + (e + 1) * 12 + 4 <= stscl)
                         ? (int)be32(stsc + 8 + (e + 1) * 12) - 1 : nchunks;
        for (int ch = first_chunk; ch <= last_chunk && s < nsamp; ch++) {
            unsigned long long coff = stco ? be32(stco + 8 + (ch - 1) * 4)
                                           : be64(co64 + 8 + (ch - 1) * 8);
            for (int j = 0; j < spc && s < nsamp; j++) { S[s].offset = (unsigned)coff; coff += S[s].size; s++; }
        }
    }
    if (stts && sttsl >= 8) {
        int ne = (int)be32(stts + 4), si = 0;
        for (int e = 0; e < ne && si < nsamp; e++) {
            int eo = 8 + e * 8; if (eo + 8 > sttsl) break;
            int cnt = (int)be32(stts + eo); unsigned dur = be32(stts + eo + 4);
            for (int j = 0; j < cnt && si < nsamp; j++) S[si++].duration = dur;
        }
    }
    if (stss && stssl >= 8) {
        int ne = (int)be32(stss + 4);
        for (int e = 0; e < ne; e++) { int eo = 8 + e * 4; if (eo + 4 > stssl) break;
            int sn = (int)be32(stss + eo); if (sn >= 1 && sn <= nsamp) S[sn - 1].keyframe = 1; }
    } else for (int i = 0; i < nsamp; i++) S[i].keyframe = 1;
    *out = S;
    return nsamp;
}

// AAC sample-rate table (ASC sampling_frequency_index).
static const int s_aac_rates[16] = {
    96000,88200,64000,48000,44100,32000,24000,22050,16000,12000,11025,8000,7350,0,0,0
};

int mp4_parse(const unsigned char *data, int len, mp4_video *out)
{
    memset(out, 0, sizeof(*out));
    int mlen = 0;
    const unsigned char *moov = find_box(data, len, "moov", &mlen);
    if (!moov) return -1;

    const unsigned char *vstbl = NULL, *astbl = NULL;
    int vstbl_len = 0, astbl_len = 0;
    unsigned vts = 0, ats = 0;
    int p = 0; const unsigned char *t, *c; int cl;
    while (next_box(moov, mlen, &p, &t, &c, &cl)) {
        if (memcmp(t, "trak", 4) != 0) continue;
        int mdial = 0; const unsigned char *mdia = find_box(c, cl, "mdia", &mdial);
        if (!mdia) continue;
        int hl = 0; const unsigned char *hdlr = find_box(mdia, mdial, "hdlr", &hl);
        if (!hdlr || hl < 12) continue;
        int is_vid = memcmp(hdlr + 8, "vide", 4) == 0;
        int is_aud = memcmp(hdlr + 8, "soun", 4) == 0;
        if (!is_vid && !is_aud) continue;
        int mdhdl = 0; const unsigned char *mdhd = find_box(mdia, mdial, "mdhd", &mdhdl);
        unsigned ts = 0;
        if (mdhd && mdhdl >= 20) ts = (mdhd[0] == 1) ? be32(mdhd + 20) : be32(mdhd + 12);
        int minfl = 0; const unsigned char *minf = find_box(mdia, mdial, "minf", &minfl);
        if (!minf) continue;
        int sl = 0; const unsigned char *stbl = find_box(minf, minfl, "stbl", &sl);
        if (!stbl) continue;
        if (is_vid && !vstbl) { vstbl = stbl; vstbl_len = sl; vts = ts; }
        else if (is_aud && !astbl) { astbl = stbl; astbl_len = sl; ats = ts; }
    }
    if (!vstbl) return -2;
    out->timescale = vts ? vts : 90000;

    // Video: stsd -> avc1 -> avcC.
    int stsdl = 0; const unsigned char *stsd = find_box(vstbl, vstbl_len, "stsd", &stsdl);
    if (!stsd || stsdl < 8) return -3;
    int avc1l = 0; const unsigned char *avc1 = find_box(stsd + 8, stsdl - 8, "avc1", &avc1l);
    if (!avc1) avc1 = find_box(stsd + 8, stsdl - 8, "avc3", &avc1l);
    if (!avc1 || avc1l < 78) return -4;
    out->width = be16(avc1 + 24); out->height = be16(avc1 + 26);
    int avcCl = 0; const unsigned char *avcC = find_box(avc1 + 78, avc1l - 78, "avcC", &avcCl);
    if (!avcC || avcCl < 7) return -5;
    out->nal_length_size = (avcC[4] & 3) + 1;
    int numsps = avcC[5] & 0x1F, q = 6;
    if (numsps >= 1 && q + 2 <= avcCl) {
        int spl = be16(avcC + q); q += 2;
        if (q + spl <= avcCl) { out->sps = malloc(spl); if (out->sps) { memcpy(out->sps, avcC + q, spl); out->sps_len = spl; } }
        q += spl;
        for (int i = 1; i < numsps && q + 2 <= avcCl; i++) { int l = be16(avcC + q); q += 2 + l; }
    }
    if (q + 1 <= avcCl) {
        int numpps = avcC[q++];
        if (numpps >= 1 && q + 2 <= avcCl) { int pl = be16(avcC + q); q += 2;
            if (q + pl <= avcCl) { out->pps = malloc(pl); if (out->pps) { memcpy(out->pps, avcC + q, pl); out->pps_len = pl; } } }
    }
    out->num_samples = build_samples(vstbl, vstbl_len, &out->samples);
    if (out->num_samples <= 0) return -6;

    // Audio: stsd -> mp4a -> esds -> AudioSpecificConfig.
    if (astbl) {
        int asdl = 0; const unsigned char *asd = find_box(astbl, astbl_len, "stsd", &asdl);
        int m4al = 0; const unsigned char *mp4a = asd && asdl > 8 ? find_box(asd + 8, asdl - 8, "mp4a", &m4al) : NULL;
        // mp4a AudioSampleEntry: channelcount@16, samplerate@24 (16.16).
        if (mp4a && m4al >= 28) {
            out->audio_channels = be16(mp4a + 16);
            out->audio_rate = be16(mp4a + 24);  // upper 16 of 16.16
            int esdsl = 0; const unsigned char *esds = find_box(mp4a + 28, m4al - 28, "esds", &esdsl);
            if (esds && esdsl > 4) {
                // Walk the ES_Descriptor tags to DecoderSpecificInfo (tag 0x05).
                const unsigned char *e = esds + 4; int er = esdsl - 4;  // skip fullbox
                while (er > 2) {
                    int tag = e[0]; e++; er--;
                    int l = 0, b; do { b = *e++; er--; l = (l << 7) | (b & 0x7F); } while ((b & 0x80) && er > 0);
                    if (tag == 0x05) {  // DecoderSpecificInfo = ASC
                        if (l > 0 && l <= er) { out->asc = malloc(l); if (out->asc) { memcpy(out->asc, e, l); out->asc_len = l; } }
                        break;
                    } else if (tag == 0x03) { e += 3; er -= 3; }  // ES_Descriptor header
                    else if (tag == 0x04) { e += 13; er -= 13; }  // DecoderConfigDescriptor
                    else { e += l; er -= l; }
                }
            }
            // Prefer the rate/channels encoded in the ASC when present.
            if (out->asc && out->asc_len >= 2) {
                int idx = ((out->asc[0] & 0x07) << 1) | (out->asc[1] >> 7);
                if (s_aac_rates[idx]) out->audio_rate = s_aac_rates[idx];
                out->audio_channels = (out->asc[1] >> 3) & 0x0F;
            }
            // Fallback when esds yields no ASC: synthesize a minimal AAC-LC config
            // from the sample-entry channels + rate.
            if (!out->asc && out->audio_channels >= 1 && out->audio_rate > 0) {
                int fi = 4;  // 44100 default
                for (int i = 0; i < 13; i++)
                    if (s_aac_rates[i] == out->audio_rate) { fi = i; break; }
                int ch = out->audio_channels; if (ch < 1 || ch > 7) ch = 2;
                out->asc = malloc(2);
                if (out->asc) {
                    out->asc[0] = (unsigned char)((2 << 3) | (fi >> 1));  // AAC-LC + freqIdx hi
                    out->asc[1] = (unsigned char)(((fi & 1) << 7) | (ch << 3));  // freqIdx lo + chans
                    out->asc_len = 2;
                }
            }
            out->audio_timescale = ats ? ats : (unsigned)out->audio_rate;
            out->num_audio_samples = build_samples(astbl, astbl_len, &out->audio_samples);
            if (out->num_audio_samples > 0 && out->asc) out->has_audio = 1;
        }
    }
    return 0;
}

void mp4_free(mp4_video *out)
{
    free(out->sps); free(out->pps); free(out->samples);
    free(out->asc); free(out->audio_samples);
    memset(out, 0, sizeof(*out));
}
