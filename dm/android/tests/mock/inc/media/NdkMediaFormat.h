#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef int media_status_t;
enum { AMEDIA_OK = 0, AMEDIA_ERROR_UNKNOWN = -10000, AMEDIA_ERROR_UNSUPPORTED = -10003 };
typedef struct AMediaFormat AMediaFormat;
AMediaFormat *AMediaFormat_new(void);
media_status_t AMediaFormat_delete(AMediaFormat *f);
bool AMediaFormat_getInt32(AMediaFormat *f, const char *k, int32_t *v);
bool AMediaFormat_getBuffer(AMediaFormat *f, const char *k, void **d, size_t *n);
bool AMediaFormat_getRect(AMediaFormat *f, const char *k, int32_t *l, int32_t *t, int32_t *r, int32_t *b);
void AMediaFormat_setInt32(AMediaFormat *f, const char *k, int32_t v);
void AMediaFormat_setString(AMediaFormat *f, const char *k, const char *v);
void AMediaFormat_setBuffer(AMediaFormat *f, const char *k, const void *d, size_t n);
void AMediaFormat_setRect(AMediaFormat *f, const char *k, int32_t l, int32_t t, int32_t r, int32_t b);
#define AMEDIAFORMAT_KEY_AAC_MAX_OUTPUT_CHANNEL_COUNT "aac-max-output-channel_count"
#define AMEDIAFORMAT_KEY_CHANNEL_COUNT "channel-count"
#define AMEDIAFORMAT_KEY_COLOR_FORMAT "color-format"
#define AMEDIAFORMAT_KEY_COLOR_RANGE "color-range"
#define AMEDIAFORMAT_KEY_COLOR_STANDARD "color-standard"
#define AMEDIAFORMAT_KEY_CSD_0 "csd-0"
#define AMEDIAFORMAT_KEY_CSD_1 "csd-1"
#define AMEDIAFORMAT_KEY_DISPLAY_CROP "crop"
#define AMEDIAFORMAT_KEY_HEIGHT "height"
#define AMEDIAFORMAT_KEY_IS_ADTS "is-adts"
#define AMEDIAFORMAT_KEY_MAX_INPUT_SIZE "max-input-size"
#define AMEDIAFORMAT_KEY_MIME "mime"
#define AMEDIAFORMAT_KEY_PCM_ENCODING "pcm-encoding"
#define AMEDIAFORMAT_KEY_SAMPLE_RATE "sample-rate"
#define AMEDIAFORMAT_KEY_SLICE_HEIGHT "slice-height"
#define AMEDIAFORMAT_KEY_STRIDE "stride"
#define AMEDIAFORMAT_KEY_WIDTH "width"
