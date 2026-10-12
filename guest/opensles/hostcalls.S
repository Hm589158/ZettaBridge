@ Audio host calls of the guest libOpenSLES.so (core/include/zb/audio_protocol.h). ARM mode:
@ a Thumb svc immediate has only 8 bits. Arguments and the result stay in r0-r2 / r0, so each
@ call is the svc and a return.
.syntax unified
.arm
.text

.macro hostcall name, index
.global \name
.hidden \name
.type \name, %function
.p2align 2
\name:
    svc #(0x5A0000 | \index)
    bx lr
.size \name, . - \name
.endm

hostcall zbsl_hc_probe, 0xFA00
hostcall zbsl_hc_pcm_open, 0xFA01
hostcall zbsl_hc_pcm_write, 0xFA02
hostcall zbsl_hc_pcm_control, 0xFA03
hostcall zbsl_hc_pcm_close, 0xFA04
hostcall zbsl_hc_media_open, 0xFA05
hostcall zbsl_hc_media_control, 0xFA06
hostcall zbsl_hc_media_query, 0xFA07
hostcall zbsl_hc_media_close, 0xFA08
