/*
Copyright (C) 2015-2017 Parallel Realities

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

#include "controls.h"
#include "../../sf3500/joykey.hpp"
#include <ctype.h>
#include <strings.h>

int isControl(int type)
{
	int key, btn;

	key = app.config.keyControls[type];
	btn = app.config.joypadControls[type];

	if (type == CONTROL_LEFT && app.joypadAxis[JOYPAD_AXIS_X] < -app.config.deadzone)
	{
		return 1;
	}

	if (type == CONTROL_RIGHT && app.joypadAxis[JOYPAD_AXIS_X] > app.config.deadzone)
	{
		return 1;
	}

	if (type == CONTROL_UP && app.joypadAxis[JOYPAD_AXIS_Y] < -app.config.deadzone)
	{
		return 1;
	}

	if (type == CONTROL_DOWN && app.joypadAxis[JOYPAD_AXIS_Y] > app.config.deadzone)
	{
		return 1;
	}

	return ((key != 0 && app.keyboard[key]) || (btn != -1 && app.joypadButton[btn]));
}

int isAcceptControl(void)
{
	/* input.c raises SDL_SCANCODE_RETURN for the buttons listed as "accept" in
	 * controls.txt (A, B and X unless changed) */
	return app.keyboard[SDL_SCANCODE_RETURN];
}

void clearControl(int type)
{
	int key;
	int btn;

	key = app.config.keyControls[type];
	btn = app.config.joypadControls[type];

	if (key != 0)
	{
		app.keyboard[key] = 0;
	}

	if (btn != 0)
	{
		app.joypadButton[btn] = 0;
	}

	if (type == CONTROL_LEFT || type == CONTROL_RIGHT)
	{
		app.joypadAxis[JOYPAD_AXIS_X] = 0;
	}

	if (type == CONTROL_UP || type == CONTROL_DOWN)
	{
		app.joypadAxis[JOYPAD_AXIS_Y] = 0;
	}
}

void clearAcceptControls(void)
{
	clearControl(CONTROL_USE);
	clearControl(CONTROL_JUMP);

	app.keyboard[SDL_SCANCODE_SPACE] = app.keyboard[SDL_SCANCODE_RETURN] = 0;
}

/* The game's text and menus talk about keyboard keys ("press [Space] to create a
 * clone"). On the handheld the same controls are buttons: input.c turns each
 * button into the key the game expects, using the mapping read here from
 * controls.txt. Every place that prints a key name goes through this file to show
 * the button instead. */
typedef struct
{
	const char *name;	/* as written in controls.txt */
	const char *label;	/* as shown on screen */
	int bit;			/* JK_* from joykey.hpp */
} ButtonName;

static const ButtonName buttonNames[] =
{
	{"LEFT", "D-Pad Left", JK_LEFT}, {"RIGHT", "D-Pad Right", JK_RIGHT}, {"UP", "D-Pad Up", JK_UP}, {"DOWN", "D-Pad Down", JK_DOWN},
	{"A", "A", JK_A}, {"B", "B", JK_B}, {"X", "X", JK_X}, {"Y", "Y", JK_Y},
	{"L1", "L1", JK_L1}, {"R1", "R1", JK_R1}, {"L2", "L2", JK_L2}, {"R2", "R2", JK_R2}, {"START", "Start", JK_START}, {"SELECT", "Select", JK_SELECT},
	{NULL, NULL, 0}
};

/* the order of the CONTROL_* values in defs.h, then "accept" */
static const char *actionNames[CONTROL_MAX + 1] =
{
	"left", "right", "up", "down", "jump", "use", "clone", "restart", "pause", "accept"
};

static const char *defaultButtons[CONTROL_MAX + 1] =
{
	"LEFT", "RIGHT", "UP", "DOWN", "A", "B", "X", "Y", "START", "A,B,X"
};

#define ACCEPT CONTROL_MAX

static unsigned int actionMask[CONTROL_MAX + 1];
static char actionLabel[CONTROL_MAX + 1][64];
static int mappingLoaded;

static char *trim(char *s)
{
	char *e;

	while (*s == ' ' || *s == '\t')
	{
		s++;
	}

	e = s + strlen(s);

	while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n'))
	{
		*--e = '\0';
	}

	return s;
}

/* "A, UP" -> mask of those buttons and the text "A / D-Pad Up"; returns 0 if a name is unknown */
static int parseButtons(const char *list, unsigned int *mask, char *label, int labelSize)
{
	char buf[128], *tok, *save = NULL;
	const ButtonName *b;

	*mask = 0;
	label[0] = '\0';

	strncpy(buf, list, sizeof(buf) - 1);
	buf[sizeof(buf) - 1] = '\0';

	for (tok = strtok_r(buf, ",", &save) ; tok != NULL ; tok = strtok_r(NULL, ",", &save))
	{
		char name[16];
		int i;

		tok = trim(tok);

		if (*tok == '\0')
		{
			continue;
		}

		for (i = 0 ; tok[i] && i < (int)sizeof(name) - 1 ; i++)
		{
			name[i] = toupper((unsigned char)tok[i]);
		}

		name[i] = '\0';

		if (strcmp(name, "NONE") == 0)
		{
			continue;
		}

		for (b = buttonNames ; b->name != NULL ; b++)
		{
			if (strcmp(b->name, name) == 0)
			{
				break;
			}
		}

		if (b->name == NULL)
		{
			return 0;
		}

		*mask |= 1u << b->bit;

		if (label[0] != '\0')
		{
			strncat(label, " / ", labelSize - strlen(label) - 1);
		}

		strncat(label, b->label, labelSize - strlen(label) - 1);
	}

	return 1;
}

static void setAction(int a, const char *list)
{
	parseButtons(list, &actionMask[a], actionLabel[a], sizeof(actionLabel[a]));
}

/* Reads controls.txt (one "action=BUTTON[,BUTTON...]" per line, # for comments)
 * from the port folder. Anything missing or wrong keeps its default, with a note
 * in the log; the pause and accept controls always keep at least one button so a
 * mistake cannot lock the menus. */
static void loadMapping(void)
{
	char line[256], label[64];
	const char *path;
	unsigned int mask;
	FILE *f;
	int a, lineNo = 0;

	mappingLoaded = 1;

	for (a = 0 ; a <= CONTROL_MAX ; a++)
	{
		setAction(a, defaultButtons[a]);
	}

	path = getenv("WC_CONTROLS") ? getenv("WC_CONTROLS") : "controls.txt";
	f = fopen(path, "r");

	if (f == NULL)
	{
		return;
	}

	while (fgets(line, sizeof(line), f))
	{
		char *key, *value, *eq;

		lineNo++;

		key = trim(line);

		if (*key == '#' || *key == '\0')
		{
			continue;
		}

		eq = strchr(key, '=');

		if (eq == NULL)
		{
			fprintf(stderr, "controls.txt line %d: expected action=BUTTON, ignored\n", lineNo);
			continue;
		}

		*eq = '\0';
		value = trim(eq + 1);
		key = trim(key);

		for (a = 0 ; a <= CONTROL_MAX ; a++)
		{
			if (strcasecmp(key, actionNames[a]) == 0)
			{
				break;
			}
		}

		if (a > CONTROL_MAX)
		{
			fprintf(stderr, "controls.txt line %d: unknown action '%s', ignored\n", lineNo, key);
		}
		else if (!parseButtons(value, &mask, label, sizeof(label)))
		{
			fprintf(stderr, "controls.txt line %d: unknown button in '%s' (use LEFT RIGHT UP DOWN A B X Y L1 R1 L2 R2 START SELECT or NONE), keeping the default for %s\n", lineNo, value, key);
		}
		else if ((a == CONTROL_PAUSE || a == ACCEPT) && mask == 0)
		{
			fprintf(stderr, "controls.txt line %d: %s needs at least one button, keeping the default\n", lineNo, key);
		}
		else
		{
			actionMask[a] = mask;
			strcpy(actionLabel[a], mask ? label : "-");
		}
	}

	fclose(f);
}

/* bit mask of the JK_* buttons that make up a control (CONTROL_*, or CONTROL_MAX for "accept") */
unsigned int getControlButtons(int control)
{
	if (!mappingLoaded)
	{
		loadMapping();
	}

	return actionMask[control];
}

const char *getControlLabel(int control)
{
	if (!mappingLoaded)
	{
		loadMapping();
	}

	return actionLabel[control];
}

/* the label for a key name as the game prints it ("Space", "Backspace" ...), or NULL */
static const char *labelForKeyName(const char *name)
{
	int c;

	for (c = 0 ; c < CONTROL_MAX ; c++)
	{
		if (app.config.keyControls[c] != 0 && strcmp(SDL_GetScancodeName(app.config.keyControls[c]), name) == 0)
		{
			return getControlLabel(c);
		}
	}

	/* the keys the menus use that are not controls of their own */
	if (strcmp(name, "Return") == 0)
	{
		return getControlLabel(ACCEPT);
	}

	if (strcmp(name, "Escape") == 0)
	{
		return getControlLabel(CONTROL_PAUSE);
	}

	return NULL;
}

/* rewrites [KeyName] in place as [Button] */
void translateKeyNames(char *buf, int size)
{
	char out[1024], name[32];
	const char *in, *label, *end;
	int n, len;

	if (strchr(buf, '[') == NULL || strlen(buf) >= sizeof(out) - 32)
	{
		return;
	}

	in = buf;
	n = 0;

	while (*in)
	{
		end = (*in == '[') ? strchr(in, ']') : NULL;
		len = end ? (int)(end - in) - 1 : 0;

		if (end && len > 0 && len < (int)sizeof(name))
		{
			memcpy(name, in + 1, len);
			name[len] = '\0';

			label = labelForKeyName(name);

			if (label && n + (int)strlen(label) + 2 < (int)sizeof(out))
			{
				out[n++] = '[';
				strcpy(out + n, label);
				n += strlen(label);
				out[n++] = ']';
				in = end + 1;

				continue;
			}
		}

		out[n++] = *in++;
	}

	out[n] = '\0';

	if (n < size)
	{
		strcpy(buf, out);
	}
}
