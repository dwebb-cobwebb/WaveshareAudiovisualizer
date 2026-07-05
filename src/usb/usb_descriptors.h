#ifndef _USB_DESCRIPTORS_H_
#define _USB_DESCRIPTORS_H_

#include <stdint.h>

// ===========================================================================
// UAC 1.0 descriptor layout for the "1U Visualizer" full-speed audio sink.
//
// UAC1 (handled by usbaudio.sys on Windows) is used instead of UAC2 because
// usbaudio2.sys issues GET_ISOCH_PIPE_TRANSFER_PATH_DELAYS for full-speed
// devices, which the test host answers NOT_SUPPORTED, so streaming never
// starts. UAC1 has no such query.
// ===========================================================================

// Interface numbers
#define ITF_NUM_AUDIO_CONTROL     0
#define ITF_NUM_AUDIO_STREAMING   1
#define ITF_NUM_TOTAL             2

// Audio entity IDs (UAC1 — no clock entity)
#define UAC1_ENTITY_INPUT_TERMINAL   0x01
#define UAC1_ENTITY_FEATURE_UNIT     0x02
#define UAC1_ENTITY_OUTPUT_TERMINAL  0x03

// Isochronous OUT endpoint (host -> device audio data)
#define EPNUM_AUDIO_OUT   0x01

// Max packet: 48 kHz * 2 ch * 2 bytes = exactly 192 B/frame. No slack — an
// adaptive sink consumes at the host's rate, and the working full-speed
// reference on the test PC (Plantronics, 192 B) proves this host schedules
// 192-byte FS iso pipes; 196 is the one remaining unexplained difference.
#define AUDIO_OUT_EP_SIZE   (2 * 2 * 48)   // 192

// Length of the whole audio function (both interfaces + all class descriptors),
// i.e. everything after the IAD. Used by the class driver's open() callback.
#define AUDIO_FUNC_DESC_LEN   101

#endif /* _USB_DESCRIPTORS_H_ */
