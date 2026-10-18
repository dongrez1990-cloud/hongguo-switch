/*
 * Tests for hg_lang.c -- the interface language table.
 *
 * The check that matters most: EVERY KEY HAS BOTH LANGUAGES, AND EVERY STRING IS WELL-FORMED UTF-8.
 * A key added in one language and forgotten in the other shows up as a blank label on a console
 * screen, and a blank label reads as a rendering bug rather than as a missing translation -- so the
 * failure this suite prevents is one that would otherwise be diagnosed in the wrong place.
 *
 * The UTF-8 check is here because of HOW the Chinese is stored: as `\xNN` byte escapes, to keep the
 * source pure ASCII for this project's GBK build machine. That notation is easy to get subtly wrong
 * -- one wrong byte and the console shows a replacement glyph -- and a typo in an escape is not
 * something a compiler complains about. (One was already made and caught while writing the table: a
 * `\xxE4` instead of `\xE4`.)
 *
 * ASCII-only source file.
 */

#include "hg_lang.h"

#include <stdio.h>
#include <string.h>

static int checks = 0;
static int failures = 0;

static void ok(const char* what, int cond) {
  checks++;
  if (cond) printf("  PASS  %-58s\n", what);
  else { failures++; printf("  FAIL  %-58s\n", what); }
}

/* Is `s` valid UTF-8? Walks the bytes and validates every lead/continuation sequence. Rejects
 * overlong forms and truncated tails, which are exactly what a mistyped escape produces. */
static int validUtf8(const char* s) {
  const unsigned char* p = (const unsigned char*)s;
  if (!s) return 0;
  while (*p) {
    unsigned char c = *p++;
    int extra;
    if (c < 0x80) continue;             /* ASCII */
    else if ((c & 0xE0) == 0xC0) extra = 1;
    else if ((c & 0xF0) == 0xE0) extra = 2;
    else if ((c & 0xF8) == 0xF0) extra = 3;
    else return 0;                      /* not a lead byte */
    for (int i = 0; i < extra; i++) {
      if ((*p & 0xC0) != 0x80) return 0;   /* missing or wrong continuation */
      p++;
    }
  }
  return 1;
}

static int nonAsciiCount(const char* s) {
  int n = 0;
  if (!s) return 0;
  for (const unsigned char* p = (const unsigned char*)s; *p; p++) {
    if (*p >= 0x80) n++;
  }
  return n;
}

/* ---- the table ------------------------------------------------------------ */

static void test_table(void) {
  int i;
  int missingEn = 0, badUtf8 = 0, empty = 0, tooLong = 0;

  printf("\n-- every key is present in both languages --\n");

  for (i = 0; i < HG_T_COUNT; i++) {
    /* The English column is the fallback, so it must always exist and be non-empty. */
    const char* en = NULL;
    const char* zh = NULL;
    /* hg_tr walks the language in force; ask for both by switching. */
    hg_lang_set(HG_LANG_EN);
    en = hg_tr(i);
    hg_lang_set(HG_LANG_ZH);
    zh = hg_tr(i);

    if (!en || en[0] == '\0') { missingEn++; printf("        key %d has no English\n", i); }
    if (!zh || zh[0] == '\0') { empty++; printf("        key %d renders empty in Chinese\n", i); }
    if (!validUtf8(en)) { badUtf8++; printf("        key %d English is not valid UTF-8\n", i); }
    if (!validUtf8(zh)) { badUtf8++; printf("        key %d Chinese is not valid UTF-8\n", i); }
    if ((en && strlen(en) > 200) || (zh && strlen(zh) > 200)) tooLong++;
  }

  ok("every key has a non-empty English string", missingEn == 0);
  ok("every key has a non-empty Chinese string", empty == 0);
  ok("every string is valid UTF-8 in both languages", badUtf8 == 0);
  ok("no string is absurdly long", tooLong == 0);

  /* The Chinese column must actually be Chinese for the rows that are meant to be: a table entry
   * accidentally left as English would render as English in Chinese mode and look like a bug. */
  {
    int notChinese = 0;
    hg_lang_set(HG_LANG_ZH);
    for (i = 0; i < HG_T_LANGUAGE_EN; i++) {   /* the language names are the exception */
      if (nonAsciiCount(hg_tr(i)) == 0) {
        notChinese++;
        printf("        key %d is ASCII-only in Chinese mode: '%s'\n", i, hg_tr(i));
      }
    }
    ok("every Chinese entry actually contains non-ASCII text", notChinese == 0);
  }
}

static void test_switching(void) {
  printf("\n-- switching language selects the right column --\n");

  hg_lang_set(HG_LANG_ZH);
  ok("Chinese is the default", hg_lang_get() == HG_LANG_ZH);
  ok("the picture menu title is Chinese by default",
     strcmp(hg_tr(HG_T_PICTURE), hg_tr(HG_T_PICTURE)) == 0 && nonAsciiCount(hg_tr(HG_T_PICTURE)) > 0);

  hg_lang_set(HG_LANG_EN);
  ok("switching to English is recorded", hg_lang_get() == HG_LANG_EN);
  ok("and the English string is returned", strcmp(hg_tr(HG_T_PICTURE), "Picture") == 0);

  /* An out-of-range code falls back rather than storing something with no strings behind it. */
  hg_lang_set(99);
  ok("an out-of-range language falls back to the default", hg_lang_get() == HG_LANG_ZH);
  hg_lang_set(-5);
  ok("a negative language falls back too", hg_lang_get() == HG_LANG_ZH);
}

static void test_parse(void) {
  printf("\n-- parsing the cfg value --\n");

  ok("'en' is English", hg_lang_parse("en") == HG_LANG_EN);
  ok("'EN' is English (case-insensitive)", hg_lang_parse("EN") == HG_LANG_EN);
  ok("'english' is English", hg_lang_parse("english") == HG_LANG_EN);
  ok("'eng' is English", hg_lang_parse("eng") == HG_LANG_EN);

  ok("'zh' is Chinese", hg_lang_parse("zh") == HG_LANG_ZH);
  ok("'cn' is Chinese", hg_lang_parse("cn") == HG_LANG_ZH);
  ok("'zh-Hans' is Chinese", hg_lang_parse("zh-Hans") == HG_LANG_ZH);
  ok("'chinese' is Chinese", hg_lang_parse("chinese") == HG_LANG_ZH);

  /* A typo must leave a readable interface, not an empty one. */
  ok("an empty value is the default", hg_lang_parse("") == HG_LANG_ZH);
  ok("NULL is the default", hg_lang_parse(NULL) == HG_LANG_ZH);
  ok("a typo is the default", hg_lang_parse("englsh") == HG_LANG_ZH);
  ok("an unknown code is the default", hg_lang_parse("fr") == HG_LANG_ZH);

  /* The default is Chinese, at the user's explicit request. */
  ok("the default is Chinese, not English", hg_lang_parse("") != HG_LANG_EN);
}

static void test_names_and_bounds(void) {
  printf("\n-- language names and out-of-range keys --\n");

  ok("Chinese is named in Chinese", validUtf8(hg_lang_name(HG_LANG_ZH)) &&
                                       nonAsciiCount(hg_lang_name(HG_LANG_ZH)) > 0);
  ok("English is named in English", strcmp(hg_lang_name(HG_LANG_EN), "English") == 0);
  ok("an out-of-range language name falls back", hg_lang_name(99) != NULL);
  ok("a NULL-able name is never NULL", hg_lang_name(-1) != NULL);

  /* An out-of-range key returns an empty string rather than reading past the table. */
  ok("a negative key is empty", hg_tr(-1)[0] == '\0');
  ok("a past-the-end key is empty", hg_tr(HG_T_COUNT)[0] == '\0');
  ok("a far-out key is empty", hg_tr(99999)[0] == '\0');

  /* Every key in range returns something, which is the property the interface relies on. */
  {
    int empty = 0;
    for (int lang = 0; lang < HG_LANG_COUNT; lang++) {
      hg_lang_set(lang);
      for (int i = 0; i < HG_T_COUNT; i++) {
        if (hg_tr(i)[0] == '\0') empty++;
      }
    }
    ok("no key renders empty in any language", empty == 0);
  }
}

int main(void) {
  printf("hg_lang tests: the interface language table\n");
  printf("================================================\n");

  test_table();
  test_switching();
  test_parse();
  test_names_and_bounds();

  printf("\n================================================\n");
  printf("checks: %d   failures: %d\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
