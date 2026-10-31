/*
Translations. Everything the game shows goes through tr(key, english): the key names
the string, and "english" is what is shown when no translation has it. The strings
live in lang/en_US.json (the master, one entry for every string in the game) and in
lang/<locale>.json for each other language; a translated entry replaces the English
one, and a missing entry simply stays English. The locale comes from the WC_LANG
environment variable (run.sh sets it from TreeFrogUI's language setting), so with it
unset the game is English and reads no language files at all.
*/

#ifndef LANG_H
#define LANG_H

void initLang(void);
const char *tr(const char *key, const char *english);
const char *langLocale(void);
void langDisable(void);

/* every string of the loaded language (English entries it does not replace included),
 * so the text code can check that its font can draw them all */
int langCount(void);
const char *langString(int i);

#endif
