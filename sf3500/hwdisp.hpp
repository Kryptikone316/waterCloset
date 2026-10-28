/* sf3000-hwdisp - hardware display via driver_sf3500.so + HCGE.
 * Wraps SF3000/SF3500 HiChip Graphics Engine for rotation+scale+present in
 * HW. This is the real hardware rotation path; SDL's own
 * SDL_VIDEO_FBCON_ROTATION renders fully black on this device. */
#ifndef SF3000_HWDISP_H
#define SF3000_HWDISP_H

#include <stddef.h>
/* Not <stdint.h> on purpose: its uintptr_t typedef conflicts with SDL1.2's
 * own SDL_config_minimal.h on this target. We only need 16-bit pixels;
 * `unsigned short` is guaranteed 16-bit on this MIPS o32 target. */

#ifdef __cplusplus
extern "C" {
#endif

int  hwdisp_init(void);
int  hwdisp_active(void);
void hwdisp_set_target_aspect(int num, int den);
void hwdisp_set_filter(int nearest);
void hwdisp_present(const void *src, int w, int h, int pitch_bytes);
void hwdisp_deinit(void);

#ifdef __cplusplus
}
#endif

#endif
