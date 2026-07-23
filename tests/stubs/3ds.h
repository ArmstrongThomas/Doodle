#ifndef DOODLE_HOST_TEST_3DS_STUB_H
#define DOODLE_HOST_TEST_3DS_STUB_H

#include <stdint.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;

// libctru HID values used by the settings/input layer. Keeping the real bit
// positions makes fixture failures representative of the target build.
#define KEY_A      (1u << 0)
#define KEY_B      (1u << 1)
#define KEY_SELECT (1u << 2)
#define KEY_START  (1u << 3)
#define KEY_DRIGHT (1u << 4)
#define KEY_DLEFT  (1u << 5)
#define KEY_DUP    (1u << 6)
#define KEY_DDOWN  (1u << 7)
#define KEY_R      (1u << 8)
#define KEY_L      (1u << 9)
#define KEY_X      (1u << 10)
#define KEY_Y      (1u << 11)

#endif
