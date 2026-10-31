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

#include "lang.h"
#include "io.h"
#include "../json/cJSON.h"

extern int fileExists(const char *filename);
extern char *readFile(const char *filename);

#define LANG_HASH_SIZE 4096     /* a power of two, well above the number of strings */
#define LANG_MAX_LOCALE 16

typedef struct
{
	char *key;
	char *value;
} LangEntry;

static LangEntry *entries;
static int numEntries, capEntries;
static short hashTable[LANG_HASH_SIZE];      /* index into entries, -1 = empty */
static char locale[LANG_MAX_LOCALE];

static unsigned hashKey(const char *key)
{
	unsigned h = 5381;

	while (*key)
	{
		h = h * 33 + (unsigned char)*key++;
	}

	return h;
}

static int findEntry(const char *key)
{
	unsigned h = hashKey(key) & (LANG_HASH_SIZE - 1);

	while (hashTable[h] != -1)
	{
		if (strcmp(entries[hashTable[h]].key, key) == 0)
		{
			return hashTable[h];
		}

		h = (h + 1) & (LANG_HASH_SIZE - 1);
	}

	return -1;
}

static void putEntry(const char *key, const char *value)
{
	int i = findEntry(key);
	unsigned h;

	if (i >= 0)
	{
		free(entries[i].value);
		entries[i].value = strdup(value);
		return;
	}

	if (numEntries >= LANG_HASH_SIZE / 2)
	{
		return;
	}

	if (numEntries == capEntries)
	{
		capEntries = capEntries ? capEntries * 2 : 256;
		entries = realloc(entries, sizeof(LangEntry) * capEntries);
	}

	entries[numEntries].key = strdup(key);
	entries[numEntries].value = strdup(value);

	h = hashKey(key) & (LANG_HASH_SIZE - 1);

	while (hashTable[h] != -1)
	{
		h = (h + 1) & (LANG_HASH_SIZE - 1);
	}

	hashTable[h] = numEntries++;
}

static int loadPack(const char *name)
{
	char path[MAX_FILENAME_LENGTH], *text;
	cJSON *root, *node;

	snprintf(path, sizeof(path), "lang/%s.json", name);

	if (!fileExists(path))
	{
		snprintf(path, sizeof(path), DATA_DIR"/lang/%s.json", name);

		if (!fileExists(path))
		{
			return 0;
		}
	}

	text = readFile(path);

	if (!text)
	{
		return 0;
	}

	root = cJSON_Parse(text);

	if (root)
	{
		for (node = root->child ; node != NULL ; node = node->next)
		{
			if (node->string && node->type == cJSON_String && node->valuestring && node->string[0] != '_')
			{
				putEntry(node->string, node->valuestring);
			}
		}

		cJSON_Delete(root);
	}

	free(text);

	return root != NULL;
}

void initLang(void)
{
	const char *e = getenv("WC_LANG");
	int i, ok = 1;

	memset(hashTable, -1, sizeof(hashTable));

	locale[0] = '\0';

	if (!e || !*e || strcmp(e, "en_US") == 0 || strlen(e) >= LANG_MAX_LOCALE)
	{
		return;
	}

	/* the locale becomes part of a file name, so only letters and an underscore */
	for (i = 0 ; e[i] ; i++)
	{
		if (!isalpha((unsigned char)e[i]) && e[i] != '_')
		{
			ok = 0;
		}
	}

	if (!ok)
	{
		return;
	}

	loadPack("en_US");

	if (loadPack(e))
	{
		snprintf(locale, sizeof(locale), "%s", e);
	}
	else
	{
		SDL_LogMessage(SDL_LOG_CATEGORY_APPLICATION, SDL_LOG_PRIORITY_WARN, "No language pack for '%s', using English", e);
	}
}

const char *tr(const char *key, const char *english)
{
	int i;

	if (numEntries == 0)
	{
		return english;
	}

	i = findEntry(key);

	return i >= 0 ? entries[i].value : english;
}

/* forget the translations: every string is English again */
void langDisable(void)
{
	numEntries = 0;
	locale[0] = '\0';
	memset(hashTable, -1, sizeof(hashTable));
}

const char *langLocale(void)
{
	return locale;
}

int langCount(void)
{
	return numEntries;
}

const char *langString(int i)
{
	return entries[i].value;
}
