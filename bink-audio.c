#include "bink-audio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <math.h>

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

#define SWAP32(x) (\
    (((x) >> 24) & 0xff) | \
    (((x) >>  8) & 0xff00) | \
    (((x) <<  8) & 0xff0000) | \
    (((x) << 24) & 0xff000000) \
)

typedef unsigned char u8;
typedef unsigned int u32;

static const int g_wmaCriticalFreqs[] = {
	100,    200,  300,  400,  510,  630,   770,   920,
	1080,  1270, 1480, 1720, 2000, 2320,  2700,  3150,
	3700,  4400, 5300, 6400, 7700, 9500, 12000, 15500,
	24500
};

static const int g_rleLengths[] = {
	2, 3, 4, 5, 6, 8, 9, 10, 11, 12, 13, 14, 15, 16, 32, 64
};

AudioSource Audio_createMemorySource(void *buffer, int offset, int size) {
    MemorySource *source = calloc(1, sizeof(MemorySource));
    source->state.type = SOURCE_MEMORY;
    source->state.sliceOffset = offset;
    source->state.sliceSize = size;
    source->buffer = buffer;
    return (AudioSource) {
        .handle = source
    };
}

AudioSource Audio_createFileSource(int fd, int offset, int size) {
    FileSource *source = calloc(1, sizeof(FileSource));
    source->state.type = SOURCE_FILE;
    source->state.sliceOffset = offset;
    source->state.sliceSize = size;
    source->fd = fd;
    return (AudioSource) {
        .handle = source
    };
}

int AudioSource_seek(AudioSource source, int offset) {
    SourceState *s = (SourceState*)source.handle;
    offset = MIN(MAX(offset, 0), s->sliceSize);

    s->position = s->sliceOffset + offset;
    if (s->type == SOURCE_FILE)
        return (int)lseek(((FileSource*)s)->fd, s->position, SEEK_SET);

    return s->position;
}

int AudioSource_read(AudioSource source, void *buf, int size) {
    SourceState *s = (SourceState*)source.handle;
    if (s->position < s->sliceOffset)
        s->position = s->sliceOffset;

    if (size <= 0)
        return 0;

    if (s->sliceSize >= 0) {
        if (s->position >= s->sliceSize)
            return -1;
        size = MIN(size, s->sliceSize - s->position);
    }

    size = MIN(size, s->sliceSize - s->position);

    if (s->type == SOURCE_MEMORY) {
        char *input = (char*)(((MemorySource*)s)->buffer);
        memcpy(buf, &input[s->position], size);
    }
    else if (s->type == SOURCE_FILE) {
        size = read(((FileSource*)s)->fd, buf, size);
    }

    s->position += size;

    return size;
}

static void seekBitReader(BinkBitReader *r, AudioSource source, int newOffset) {
    int curOffset = ((SourceState*)source.handle)->position;
    if (newOffset >= curOffset - r->scLen && newOffset < curOffset) {
        r->pos = (newOffset - curOffset + r->scLen) * 8;
    } else {
        AudioSource_seek(source, newOffset);
        r->pos = 0;
    }
}

static u32 getBitsLsb(int size, BinkBitReader *r, AudioSource source) {
    if (size <= 0 || size > 32)
        return 0;

    u32 value = 0;
    int remaining = size;
    while (remaining > 0) {
        if (r->pos == 0) {
            AudioSource_read(source, r->scratch, r->scLen);
        }
        u8 c = ((u8*)r->scratch)[r->pos >> 3];
        int bits = MIN(remaining, 8 - (r->pos & 7));
        value |= (u32)((c >> (r->pos & 7)) & ((1 << bits) - 1)) << (size - remaining);
        r->pos = (r->pos + bits) % (r->scLen * 8);
        r->total += bits;
        remaining -= bits;
    }

    return value;
}

void alignBitReader(BinkBitReader *r, int bits) {
    int count = (bits - (r->pos % bits)) % bits;
    r->pos = (r->pos + count) % (r->scLen * 8);
    r->total += count;
}

float getFloat(BinkBitReader *r, AudioSource source) {
    int pos = r->pos;
    unsigned data = getBitsLsb(29, r, source);
    if (data == 0)
        return 0.0f;
    unsigned sign = (data >> 28) & 1;
    unsigned mantissa = ((data >> 5) & 0x3fffff) << 1;
    unsigned exp = data & 0x1f;
    if (exp == 0)
        return 0.0f;
    unsigned result = (sign << 31) | (((exp - 23) + 149) << 23) | mantissa;
    return *((float*)&result);
}

int Audio_openTrack(BinkAudioTrack *track, AudioSource source) {
    char header[50];
    AudioSource_seek(source, 0);
    AudioSource_read(source, header, 50);

    int nSegments = *(int*)(&header[8]);
    int nTracks = *(short*)(&header[40]);
    int nChannels = header[46] & 0xff;
    int rate = *(unsigned short*)(&header[48]);

    int tableOff = 0x2c + 0xc * nTracks;
    AudioSource_seek(source, tableOff);

    int *segmentTable = (int*)calloc(nSegments, sizeof(int));
    AudioSource_read(source, segmentTable, nSegments * sizeof(int));

    int frameBits = 11;
	if (rate < 22050)
		frameBits = 9;
	else if (rate < 44100)
		frameBits = 10;

    {
        int temp = nChannels >> 1;
        while (temp) {
            temp >>= 1;
	        frameBits++;
        }
    }

	int frameSize = 1 << frameBits;
	//int overlapLen = frameSize / 16;
	int rateHalf = (rate * nChannels + 1) / 2;

	int nBands = 1;
	for (int i = 0; i < sizeof(g_wmaCriticalFreqs) / sizeof(int); i++) {
		if (rateHalf <= g_wmaCriticalFreqs[i])
			break;
		nBands++;
	}

    int *bands = (int*)calloc(nBands + 1, sizeof(int));
	bands[0] = 2;
	for (int i = 1; i < nBands; i++)
		bands[i] = (int)((double)g_wmaCriticalFreqs[i-1] * frameSize / rateHalf) & ~1;
	bands[nBands] = frameSize;

    memset(track, 0, sizeof(BinkAudioTrack));
    track->source = source;
    track->nSegments = nSegments;
    track->nTracks = nTracks;
    track->nChannels = nChannels;
    track->sampleRate = rate;
    track->frameSizeBits = frameBits;
    track->nBands = nBands;
    track->bands = bands;
    track->segmentTable = segmentTable;
    return 0;
}

int Audio_getPacketSize(BinkAudioTrack *track, int packetIndex) {
    if (packetIndex < 0 || packetIndex >= track->nSegments)
        return 0;

    int offset = track->segmentTable[packetIndex];
    AudioSource_seek(track->source, offset & ~1);
    u32 packetSize = 0;
    AudioSource_read(track->source, &packetSize, sizeof(u32));
    return (int)packetSize;
}

int Audio_initPlayback(BinkPlayback *ctx, BinkAudioTrack *track) {
    memset(ctx, 0, sizeof(BinkPlayback));
    ctx->track = track;
    // +1 to provide space for irfft algorithm, +1 again to keep two buffers in memory for cross-fade window overlap
    ctx->scratch = calloc(1 << (track->frameSizeBits + 2), sizeof(float));
    ctx->reader.scratch = calloc(4096, 1);
    ctx->reader.scLen = 4096;
    ctx->sampleOffset = 0;
    ctx->segmentIndex = 0;
    ctx->frameNumber = 0;
    ctx->framesInSegment = 0;
}

#define EXP_FACTOR 0.15289164787221953823f

static int counter = 0;

int Audio_decodeNextFrame(BinkPlayback *ctx, float *buffer) {
    BinkAudioTrack *tk = ctx->track;
    if (ctx->segmentIndex < 0 || ctx->segmentIndex >= tk->nSegments || ctx->segmentSize < 0) {
        return -1;
    }

    int frameSize = 1 << tk->frameSizeBits;
    int blockSize = 15 << (tk->frameSizeBits - 4);

    if (ctx->frameNumber == 0) {
        int initialOffset = tk->segmentTable[ctx->segmentIndex];
        //int isKeySegment = initialOffset & 1;
        initialOffset &= ~1;
        int curOffset = ((SourceState*)tk->source.handle)->position;
        /* printf(
            "%d / %d: curOffset: %d, newOffset: %d, scLen: %d, prevTotal: %d\n",
            ctx->segmentIndex, tk->nSegments, curOffset, initialOffset, ctx->reader.scLen, ctx->reader.total
        ); */
        if (ctx->segmentIndex == 0) {
            AudioSource_seek(tk->source, initialOffset);
            ctx->reader.pos = 0;
        } else {
            seekBitReader(&ctx->reader, tk->source, initialOffset);
        }

        ctx->reader.total = 0;

        ctx->segmentSize = getBitsLsb(32, &ctx->reader, tk->source);
        ctx->nFinishedSamples = getBitsLsb(32, &ctx->reader, tk->source);

        if (ctx->segmentSize <= 0 || ctx->nFinishedSamples <= 0 || ctx->segmentSize >= (1 << 28)) {
            /* printf(
                "Exiting %d: offset: %#x, segmentSize: %d, nFinishedSamples: %d\n",
                ctx->segmentIndex, initialOffset, ctx->segmentSize, ctx->nFinishedSamples
            ); */
            ctx->segmentSize = -1;
            return -1;
        }

        /* printf(
            "%d: offset: %#x, segmentSize: %d, nFinishedSamples: %d\n",
            ctx->segmentIndex, initialOffset, ctx->segmentSize, ctx->nFinishedSamples
        ); */
    }

    int nSamples = (ctx->nFinishedSamples / 30) * 32;

    float root = (float)(2.0 / (sqrt((double)frameSize) * 32768.0));

    /* printf(
        "segmentSize: %d, blockSize: %d, frameSize: %d, nSamples: %d\n",
        ctx->segmentSize, blockSize, frameSize, nSamples
    ); */

    float quantTable[28]; // can only be as large as g_wmaCriticalFreqs

    float first  = getFloat(&ctx->reader, tk->source) * root;
    float second = getFloat(&ctx->reader, tk->source) * root;

    int pos = 0;
    for (int i = 0; i < tk->nBands - 1; i++) {
        pos = getBitsLsb(8, &ctx->reader, tk->source);
        if (pos > 95)
            pos = 95;
        quantTable[i] = expf(pos * EXP_FACTOR) * root;
    }
    quantTable[tk->nBands - 1] = expf(pos * EXP_FACTOR) * root;

    int idx = 0;

    buffer[idx++] = first;
    buffer[idx++] = second;

    int b = 0;
    float q = quantTable[b];

    while (idx < nSamples && idx < frameSize && ctx->reader.total < ctx->segmentSize * 8) {
	    int isRepeat = getBitsLsb(1, &ctx->reader, tk->source);
	    int run = 1;
	    if (isRepeat) {
		    int value = getBitsLsb(4, &ctx->reader, tk->source);
		    run = g_rleLengths[value];
	    }

	    int end = MIN(MIN(idx + run*8, frameSize), nSamples);

	    int width = getBitsLsb(4, &ctx->reader, tk->source);
	    if (width > 0) {
		    while (idx < end) {
			    if (tk->bands[b] == idx && b < tk->nBands) {
				    q = quantTable[b];
				    b++;
			    }

			    int coeff = getBitsLsb(width, &ctx->reader, tk->source);
			    float value = 0;
			    if (coeff) {
				    int isNegative = (idx & 1) ^ getBitsLsb(1, &ctx->reader, tk->source);
				    value = q * (float)(isNegative ? -coeff : coeff);
			    }
			    buffer[idx++] = value;
		    }
	    }
	    else {
	        memset(&buffer[idx], 0, (end - idx) * sizeof(float));
		    idx = end;
		    while (tk->bands[b] < idx && b < tk->nBands)
			    q = quantTable[b++];
	    }
    }

    /* printf(
        "%d %d: idx = %d, nSamples = %d, frameSize = %d, reader = %d, segmentSize = %d\n",
        ctx->segmentIndex, ctx->frameNumber, idx, nSamples, frameSize, ctx->reader.total / 8, ctx->segmentSize
    ); */

    alignBitReader(&ctx->reader, 32);

    if (idx < frameSize)
        memset(&buffer[idx], 0, (frameSize - idx) * sizeof(float));

    /*
    char fname[64];
    snprintf(fname, 64, "frames/c-%d.bin", counter);
    FILE *f = fopen(fname, "wb");
    fwrite(buffer, sizeof(float), frameSize, f);
    fclose(f);
    */

    buffer[frameSize] = buffer[1];
    buffer[frameSize+1] = 0;
    buffer[1] = 0;

    int frameCount = ((ctx->nFinishedSamples / 2) + blockSize - 1) / blockSize;

    ctx->frameNumber++;
    if (ctx->frameNumber >= frameCount) {
        ctx->frameNumber = 0;
        ctx->segmentIndex++;
    }

    return 0;
}

static void do_inverse_fourier(float *buffer, int frameSizeBits) {
    int frameSize = 1 << frameSizeBits;

    for (int i = 2; i < frameSize; i += 2) {
        buffer[frameSize + i] = buffer[frameSize - i];
        buffer[frameSize + i + 1] = -buffer[frameSize - i + 1];
    }
    for (int i = 0; i < frameSize; i++) {
        int rev = 0;
        for (int j = 0; j < frameSizeBits; j++) {
            rev = (rev << 1) | ((i >> j) & 1);
        }
        if (i < rev) {
            float a = buffer[rev * 2];
            float b = buffer[rev * 2 + 1];
            buffer[rev * 2] = buffer[i * 2];
            buffer[rev * 2 + 1] = buffer[i * 2 + 1];
            buffer[i * 2] = a;
            buffer[i * 2 + 1] = b;
        }
    }

    for (int b = 0; b < frameSizeBits; b++) {
        int s = 1 << (b+1);
        double angle = 2.0 * M_PI / (double)s;
        float dx = (float)cos(angle);
        float dy = (float)sin(angle);
        for (int i = 0; i < frameSize; i += s) {
            float wx = 1.0f;
            float wy = 0.0f;
            for (int j = 0; j < s / 2; j++) {
                float *even = &buffer[2*(i+j)];
                float *odd = &even[s];
                float mr = odd[0] * wx - odd[1] * wy;
                float mi = odd[0] * wy + odd[1] * wx;
                odd[0] = even[0] - mr;
                odd[1] = even[1] - mi;
                even[0] += mr;
                even[1] += mi;
                float x = wx * dx - wy * dy;
                float y = wx * dy + wy * dx;
                wx = x;
                wy = y;
            }
        }
    }

    for (int i = 1; i < frameSize; i++)
        buffer[i] = buffer[2*i] * 0.5f;

    /*
    char fname[64];
    snprintf(fname, 64, "frames/s-%d.bin", counter);
    FILE *f = fopen(fname, "wb");
    fwrite(buffer, sizeof(float), frameSize, f);
    fclose(f);
    counter++;
    */
}

int Audio_readStream(BinkPlayback *ctx, float *buffer, int size) {
    if (size <= 0)
        return 0;

    int frameSizeBits = ctx->track->frameSizeBits;
    int frameSize = 1 << frameSizeBits;
    int overlapSize = 1 << (frameSizeBits - 4);
    int blockSize = frameSize - overlapSize;
    int scratchSize = 2 * frameSize;

    int lastRes = 0;
    int outPos = 0;
    while (outPos < size) {
        int off = ctx->sampleOffset + outPos;
        int idx = off / blockSize;
        int pos = off % blockSize;
        float *cur = &ctx->scratch[(idx & 1) * scratchSize];
        if (pos == 0) {
            lastRes = Audio_decodeNextFrame(ctx, cur);
            if (lastRes == -1)
                break;
            do_inverse_fourier(cur, frameSizeBits);
        }

        int toCopy = MIN(size - outPos, blockSize - pos);
        if (pos < overlapSize) {
            float *prev = &ctx->scratch[((idx & 1) ^ 1) * scratchSize];
            int end = MIN(pos + toCopy, overlapSize);
            for (int i = pos; i < end; i++) {
                float f = (float)i / (float)end;
                buffer[outPos-pos+i] = cur[i] * f + prev[i + blockSize] * (1.0f - f);
            }
            outPos += end - pos;
            toCopy -= end - pos;
            pos = end;
        }
        if (toCopy > 0) {
            memcpy(&buffer[outPos], &cur[pos], toCopy * sizeof(float));
            outPos += toCopy;
        }
    }

    if (outPos == 0)
        return -1;

    ctx->sampleOffset += outPos;
    return outPos;
}
