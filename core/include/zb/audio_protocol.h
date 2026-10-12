#pragma once

/* Audio host calls between the guest libOpenSLES.so (guest/opensles/zbopensles.c) and HostAudio.
 * C-compatible: the arm32 guest includes this header too.
 *
 * Host-call indices 0xFA00-0xFAFF belong to audio. tools/gen_stubs.py keeps generated stub
 * indices below this range. A host without an audio backend leaves PROBE unanswered (r0 = 0), and
 * the guest library then stays silent, pacing its buffer queues by the clock alone. */

#include <stdint.h>

#define ZB_AUDIO_HOST_CALL_FIRST 0xFA00u
#define ZB_AUDIO_HOST_CALL_LAST 0xFAFFu

/* () -> ZB_AUDIO_PROBE_MAGIC when real output is available. */
#define ZB_AUDIO_HC_PROBE 0xFA00u
/* (sample_rate_hz, channels, ZB_AUDIO_ENCODING_*) -> stream handle, 0 on failure. */
#define ZB_AUDIO_HC_PCM_OPEN 0xFA01u
/* (handle, guest buffer, bytes) -> bytes played. Blocks until the device took them all, or
 * returns early (fewer bytes) once the stream is paused, flushed or closed from another thread. */
#define ZB_AUDIO_HC_PCM_WRITE 0xFA02u
/* (handle, ZB_AUDIO_OP_*, argument) -> 0, or a negative value on failure. */
#define ZB_AUDIO_HC_PCM_CONTROL 0xFA03u
/* (handle) -> 0. */
#define ZB_AUDIO_HC_PCM_CLOSE 0xFA04u
/* (guest struct zb_audio_media*) -> media handle, 0 on failure. */
#define ZB_AUDIO_HC_MEDIA_OPEN 0xFA05u
/* (handle, ZB_AUDIO_OP_*, argument) -> 0, or a negative value on failure. */
#define ZB_AUDIO_HC_MEDIA_CONTROL 0xFA06u
/* (handle, ZB_AUDIO_QUERY_*) -> value. */
#define ZB_AUDIO_HC_MEDIA_QUERY 0xFA07u
/* (handle) -> 0. */
#define ZB_AUDIO_HC_MEDIA_CLOSE 0xFA08u

#define ZB_AUDIO_PROBE_MAGIC 0x5A415544u /* "ZAUD" */

/* Interleaved little-endian PCM sample encodings. */
#define ZB_AUDIO_ENCODING_U8 1u
#define ZB_AUDIO_ENCODING_S16 2u
#define ZB_AUDIO_ENCODING_FLOAT 3u

/* Control operations. Not every one applies to both stream kinds. */
#define ZB_AUDIO_OP_START 1u /* pcm: start or resume */
#define ZB_AUDIO_OP_PAUSE 2u /* pcm: pause, keeping queued samples */
#define ZB_AUDIO_OP_FLUSH 3u /* pcm: pause and drop queued samples */
#define ZB_AUDIO_OP_GAIN 4u  /* both: argument is a float bit pattern, 0.0 (silent) to 1.0 */
#define ZB_AUDIO_OP_PLAY_STATE 5u /* media: argument is SL_PLAYSTATE_* (1 stopped, 2 paused, 3 playing) */
#define ZB_AUDIO_OP_SEEK 6u       /* media: argument is a position in milliseconds */
#define ZB_AUDIO_OP_LOOP 7u       /* media: argument is 0 or 1 */

/* Media queries. */
#define ZB_AUDIO_QUERY_POSITION 1u /* milliseconds */
#define ZB_AUDIO_QUERY_DURATION 2u /* milliseconds, 0xFFFFFFFF while unknown */
#define ZB_AUDIO_QUERY_STATUS 3u   /* ZB_AUDIO_STATUS_* bits */

#define ZB_AUDIO_STATUS_PREFETCHED 1u /* enough data decoded to play */
#define ZB_AUDIO_STATUS_ENDED 2u      /* the head reached the end; cleared by this query */
#define ZB_AUDIO_STATUS_ERROR 4u      /* the content cannot be played */

#define ZB_AUDIO_MEDIA_FD 1u
#define ZB_AUDIO_MEDIA_URI 2u

/* A compressed source the host decodes itself (OpenSL ES URI or Android file descriptor). */
struct zb_audio_media {
    uint32_t kind;      /* ZB_AUDIO_MEDIA_* */
    int32_t fd;         /* FD: a descriptor of this process; the guest keeps ownership */
    uint32_t offset_lo; /* FD: byte offset and length; a length of all ones means "to the end" */
    uint32_t offset_hi;
    uint32_t length_lo;
    uint32_t length_hi;
    uint32_t uri;       /* URI: guest address of a NUL-terminated string */
};
