/*
Copyright (C) 2019 Parallel Realities

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/

#include "input.h"

#include "../../sf3500/joykey.hpp"
#include "../../sf3500/hwdisp.hpp"
#include "../../sf3500/sf3500_mixer.hpp"

/* No real SDL event source on this device (no window, no SDL joystick) -
 * app.keyboard[]/app.joypadAxis[] are the game's own input state (read by
 * isControl() in controls.c), so we just drive them directly from the
 * gamepad each frame instead of pumping SDL events. Scancode numbers match
 * this game's own default data/config/config.json keyControls (left=4,
 * right=7, jump=12, use=13, clone=44/space, restart=42/backspace,
 * pause=41/escape) - not reading the config at runtime since nothing on
 * this device will ever hand the player a different keyboard to remap it
 * with. */
#define SC_JUMP     12
#define SC_USE      13
#define SC_CLONE    44
#define SC_RESTART  42
#define SC_PAUSE    41

static void doExit(void)
{
	hwdisp_deinit();
	sf3500_mixer_deinit();
	exit(0);
}

void doInput(void)
{
	unsigned int jk;

	jk = joykey_read();

	/* standing failsafe exit convention on this device */
	if ((jk & ((1<<JK_SELECT)|(1<<JK_START))) == ((1<<JK_SELECT)|(1<<JK_START)))
	{
		doExit();
	}

	app.joypadAxis[JOYPAD_AXIS_X] = (jk & (1<<JK_LEFT)) ? -32768 : (jk & (1<<JK_RIGHT)) ? 32767 : 0;
	app.joypadAxis[JOYPAD_AXIS_Y] = (jk & (1<<JK_UP)) ? -32768 : (jk & (1<<JK_DOWN)) ? 32767 : 0;

	app.keyboard[SC_JUMP]    = (jk & (1<<JK_A)) != 0;
	app.keyboard[SC_USE]     = (jk & (1<<JK_B)) != 0;
	app.keyboard[SC_CLONE]   = (jk & (1<<JK_X)) != 0;
	app.keyboard[SC_RESTART] = (jk & (1<<JK_Y)) != 0;
	app.keyboard[SC_PAUSE]   = (jk & (1<<JK_START)) != 0;
}
