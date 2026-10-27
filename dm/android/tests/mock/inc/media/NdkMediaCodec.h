// The subset of the NDK's MediaCodec API the movie decoders use, for the Mac-side fake (mock_ndk.c): same names, types and values as <media/NdkMediaCodec.h>
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>
#include "NdkMediaFormat.h"
typedef struct AMediaCodec AMediaCodec;
typedef struct { int32_t offset; int32_t size; int64_t presentationTimeUs; uint32_t flags; } AMediaCodecBufferInfo;
enum { AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM = 4 };
enum { AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED = -3, AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED = -2, AMEDIACODEC_INFO_TRY_AGAIN_LATER = -1 };
AMediaCodec *AMediaCodec_createDecoderByType(const char *mime);
media_status_t AMediaCodec_getName(AMediaCodec *mc, char **out);
void AMediaCodec_releaseName(AMediaCodec *mc, char *name);
media_status_t AMediaCodec_configure(AMediaCodec *mc, const AMediaFormat *f, void *surface, void *crypto, uint32_t flags);
media_status_t AMediaCodec_start(AMediaCodec *mc);
media_status_t AMediaCodec_stop(AMediaCodec *mc);
media_status_t AMediaCodec_delete(AMediaCodec *mc);
media_status_t AMediaCodec_flush(AMediaCodec *mc);
ssize_t AMediaCodec_dequeueInputBuffer(AMediaCodec *mc, int64_t timeout_us);
uint8_t *AMediaCodec_getInputBuffer(AMediaCodec *mc, size_t idx, size_t *size);
media_status_t AMediaCodec_queueInputBuffer(AMediaCodec *mc, size_t idx, off_t off, size_t size, uint64_t time, uint32_t flags);
ssize_t AMediaCodec_dequeueOutputBuffer(AMediaCodec *mc, AMediaCodecBufferInfo *info, int64_t timeout_us);
uint8_t *AMediaCodec_getOutputBuffer(AMediaCodec *mc, size_t idx, size_t *size);
media_status_t AMediaCodec_releaseOutputBuffer(AMediaCodec *mc, size_t idx, bool render);
AMediaFormat *AMediaCodec_getOutputFormat(AMediaCodec *mc);
