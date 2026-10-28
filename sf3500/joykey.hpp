/* Real gamepad input via cubevol's /tmp/joy_key shared memory - the same
 * mechanism picoarch/FrogUI/the stock apps use on this device. No uinput, no
 * SDL joystick enumeration. */
#ifndef SF3000_JOYKEY_H
#define SF3000_JOYKEY_H

enum {
    JK_LEFT, JK_RIGHT, JK_UP, JK_DOWN,
    JK_A, JK_B, JK_L1, JK_R1, JK_X, JK_Y,
    JK_START, JK_SELECT, JK_COUNT
};

#ifdef __cplusplus
extern "C" {
#endif

/* Attaches the shared memory segment. Safe to call once at startup; silently
 * leaves the reader inert (always returns 0) if unavailable. */
void joykey_init(void);

/* Current button state, one bit per JK_* enum value (1 = held). */
unsigned int joykey_read(void);

#ifdef __cplusplus
}
#endif

#endif
