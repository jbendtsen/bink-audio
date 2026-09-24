#pragma once

#define SOURCE_MEMORY 1
#define SOURCE_FILE   2

typedef struct {
    int type;
    int sliceOffset;
    int sliceSize;
    int position;
} SourceState;

typedef struct {
    SourceState state;
    void *buffer;
} MemorySource;

typedef struct {
    SourceState state;
    int fd;
} FileSource;

typedef struct {
    void *handle;
} AudioSource;

typedef struct {
    AudioSource source;
    int nBands;
    int nSegments;
    int nTracks;
    int nChannels;
    int sampleRate;
    int frameSizeBits;
    int *segmentTable;
    int *bands;
} BinkAudioTrack;

typedef struct {
    unsigned char *scratch;
    long long int total;
    int scLen;
    int pos;
} BinkBitReader;

typedef struct {
    BinkAudioTrack *track;
    float *scratch;
    BinkBitReader reader;
    int sampleOffset;
    int nFinishedSamples;
    int segmentSize;
    int segmentIndex;
    int frameNumber;
    int framesInSegment;
} BinkPlayback;

int AudioSource_seek(AudioSource source, int offset);
int AudioSource_read(AudioSource source, void *buf, int size);
AudioSource Audio_createMemorySource(void *buffer, int offset, int size);
AudioSource Audio_createFileSource(int fd, int offset, int size);

int Audio_openTrack(BinkAudioTrack *track, AudioSource source);
int Audio_initPlayback(BinkPlayback *ctx, BinkAudioTrack *track);
int Audio_decodeNextFrame(BinkPlayback *ctx, float *buffer);
int Audio_readStream(BinkPlayback *ctx, float *buffer, int size);
