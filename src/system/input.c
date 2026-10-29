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
extern unsigned int getControlButtons(int control);
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

/* Buttons act like SDL key events: the key flag is set once when a button goes
 * down and cleared when it comes up, and in between it is left alone. The game
 * clears a flag itself once it has acted on it (a tip advanced, a menu item
 * chosen, a jump made), and that has to stick until the next press. Rewriting the
 * flag from the held button every frame would undo that after one frame, so a
 * single tap of A (several frames long at a fast frame rate) would skip through
 * every tip.
 *
 * Which buttons make up each control comes from controls.txt (controls.c). A
 * control can have several buttons; it counts as pressed while any of them is. */
static unsigned int previousButtons;
static int buttonsKnown;

static void followControl(unsigned int jk, int control, int scancode)
{
	unsigned int mask = getControlButtons(control);
	int now = (jk & mask) != 0, before = (previousButtons & mask) != 0;

	if (now != before)
	{
		app.keyboard[scancode] = now;
	}
}

/* The D-pad works the same way as a joystick's axis events: an axis is only set
 * when its buttons change. The menus clear an axis once they have moved the
 * cursor, and setting it again every frame while the pad is held would move the
 * cursor once per frame. */
static void followAxis(unsigned int jk, int negative, int positive, int axis)
{
	unsigned int mn = getControlButtons(negative), mp = getControlButtons(positive);
	int now = (jk & mn) ? -32768 : (jk & mp) ? 32767 : 0;
	int before = (previousButtons & mn) ? -32768 : (previousButtons & mp) ? 32767 : 0;

	if (now != before)
	{
		app.joypadAxis[axis] = now;
	}
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

	/* A button already held when the game starts (the one that launched it) is
	 * not a press. */
	if (!buttonsKnown)
	{
		previousButtons = jk;
		buttonsKnown = 1;
	}

	followAxis(jk, CONTROL_LEFT, CONTROL_RIGHT, JOYPAD_AXIS_X);
	followAxis(jk, CONTROL_UP, CONTROL_DOWN, JOYPAD_AXIS_Y);

	followControl(jk, CONTROL_JUMP, SC_JUMP);
	followControl(jk, CONTROL_USE, SC_USE);
	followControl(jk, CONTROL_CLONE, SC_CLONE);
	followControl(jk, CONTROL_RESTART, SC_RESTART);
	followControl(jk, CONTROL_PAUSE, SC_PAUSE);
	followControl(jk, CONTROL_MAX, SDL_SCANCODE_RETURN);	/* "accept": menus and tips */

	previousButtons = jk;
}
