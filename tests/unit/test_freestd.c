#include "oops/freestd.h"
#include "tests/test_common.h"

static void test_freestd_strings(void) {
  ASSERT_EQ(obs_strlen("hello"), 5);
  ASSERT_EQ(obs_strlen(""), 0);
  ASSERT_EQ(obs_strlen(NULL), 0);

  ASSERT_EQ(obs_strcmp("abc", "abc"), 0);
  ASSERT_TRUE(obs_strcmp("abc", "abd") < 0);
  ASSERT_TRUE(obs_strcmp("abd", "abc") > 0);

  ASSERT_EQ(obs_strncmp("abcdef", "abcxyz", 3), 0);
  ASSERT_TRUE(obs_strncmp("abcdef", "abcxyz", 4) < 0);

  char dest[16];
  obs_strncpy(dest, "test", sizeof(dest));
  ASSERT_EQ(obs_strcmp(dest, "test"), 0);
}

static void test_freestd_formatting(void) {
  char buf[64];
  size_t len;

  len = obs_format_u64(buf, 12345);
  buf[len] = '\0';
  ASSERT_EQ(obs_strcmp(buf, "12345"), 0);

  len = obs_format_i64(buf, -42);
  buf[len] = '\0';
  ASSERT_EQ(obs_strcmp(buf, "-42"), 0);

  len = obs_format_hex(buf, 0x1a2b);
  buf[len] = '\0';
  ASSERT_EQ(obs_strcmp(buf, "0x1a2b"), 0);
}

/* NID hashing, pinned against SELFish (the format authority): SHA-1(name ||
 * 16-byte suffix 518D64A635DED8C1E6B039B1C3E55230), first 8 digest bytes read
 * little-endian, shifted left 2, emitted as 11 six-bit groups MSB-first through
 * base64 with + and - as the last two symbols. The length-only check could not
 * fail on a wrong suffix, byte order or alphabet; these values can.
 * sceKernelGetProcessId is the string obSCEne has not found the platform to
 * export, but the hash of the string is still well-defined and is what this
 * pins. (SELFish commit 6aa50c8, crates/selfish-nid/tests/depended_on.rs) */
static void test_freestd_nid(void) {
  char nid[12];

  obs_compute_nid("sceKernelLoadStartModule", nid);
  ASSERT_EQ(obs_strlen(nid), 11);
  ASSERT_STR_EQ(
      nid,
      "wzvqT4UqKX8"); /* one pair that breaks on any of the four mistakes */

  obs_compute_nid("sceKernelGetProcessId", nid);
  ASSERT_STR_EQ(nid, "ciYaJofC6tg");

  obs_compute_nid("sceKernelWrite", nid);
  ASSERT_STR_EQ(nid, "4wSze92BhLI");

  obs_compute_nid("scePadReadState", nid);
  ASSERT_STR_EQ(nid, "YndgXqQVV7c");
}

static void test_freestd_snprintf(void) {
  char buf[128];
  int ret;

  /* Basic strings and numbers */
  ret = oops_snprintf(buf, sizeof(buf), "Hello %s, num=%d, hex=0x%x", "world", 42, 0xabcd);
  ASSERT_EQ(ret, 31);
  ASSERT_STR_EQ(buf, "Hello world, num=42, hex=0xabcd");

  /* Padding and alignment */
  ret = oops_snprintf(buf, sizeof(buf), "%08x|%-6s|%4d", 0x1234, "pad", 7);
  ASSERT_STR_EQ(buf, "00001234|pad   |   7");

  /* Large unsigned and pointer */
  ret = oops_snprintf(buf, sizeof(buf), "%llu|%p", 12345678901234ULL, (void *)(uintptr_t)0xdeadbeef);
  ASSERT_STR_EQ(buf, "12345678901234|0xdeadbeef");

  /* Truncation safety: buffer size 6 */
  char small[6];
  ret = oops_snprintf(small, sizeof(small), "123456789");
  ASSERT_EQ(ret, 9);
  ASSERT_STR_EQ(small, "12345");

  /* Null buffer gives required size */
  ret = oops_snprintf(NULL, 0, "Test %d", 100);
  ASSERT_EQ(ret, 8);
}

/* The set functions and the tokeniser, which are `<libc/string.h>`'s `strspn`, `strcspn`,
 * `strpbrk`, `strtok` and `strtok_r`. The edges checked are the ones a parser meets on its
 * first bad line: an empty set, a string that is all delimiters, and a trailing delimiter. */
static void test_freestd_sets_and_tokens(void) {
  ASSERT_EQ((int)obs_strspn("  \tab", " \t"), 3);
  ASSERT_EQ((int)obs_strspn("ab", ""), 0);   /* the empty set contains nothing */
  ASSERT_EQ((int)obs_strspn("", " "), 0);
  ASSERT_EQ((int)obs_strcspn("abc=1", "="), 3);
  ASSERT_EQ((int)obs_strcspn("abc", "="), 3); /* no hit is the whole string */
  ASSERT_TRUE(obs_strpbrk("abc=1", "=") != NULL);
  ASSERT_EQ(*obs_strpbrk("abc=1", "0123456789"), '1');
  ASSERT_TRUE(obs_strpbrk("abc", "xyz") == NULL);

  {
    /* The shape of an OBJ face line, tokenised the way a loader does it. */
    char line[] = "v  1.0 -2.5   3.0 ";
    char *save = NULL;
    char *t = obs_strtok_r(line, " ", &save);
    ASSERT_STR_EQ(t, "v");
    t = obs_strtok_r(NULL, " ", &save);
    ASSERT_STR_EQ(t, "1.0");            /* the run of spaces is one separator */
    t = obs_strtok_r(NULL, " ", &save);
    ASSERT_STR_EQ(t, "-2.5");
    t = obs_strtok_r(NULL, " ", &save);
    ASSERT_STR_EQ(t, "3.0");
    ASSERT_TRUE(obs_strtok_r(NULL, " ", &save) == NULL); /* the trailing space is not a token */
    ASSERT_TRUE(obs_strtok_r(NULL, " ", &save) == NULL); /* and it stays ended */
  }
  {
    char all[] = "   ";
    char *save = NULL;
    ASSERT_TRUE(obs_strtok_r(all, " ", &save) == NULL);
  }
}

/* `obs_qsort` and `obs_bsearch`, which are `<libc/stdlib.h>`'s.
 *
 * **The inputs that matter are the ordered ones.** A naive quicksort recurses `count` deep on a
 * sorted array, which is exactly what a depth-sorted scene hands it on the frame after it
 * sorted - so the sorted, reversed and all-equal cases are here, at a size past the insertion
 * sort's sixteen, and the count of comparisons is checked to stay near n log n rather than n^2.
 */
static int fs_cmp_calls;

static int fs_cmp_int(const void *a, const void *b) {
  const int x = *(const int *)a, y = *(const int *)b;
  fs_cmp_calls++;
  return (x > y) - (x < y);
}

static void test_freestd_qsort_and_bsearch(void) {
  enum { N = 300 };
  static int v[N];

  /* Already sorted - the quadratic case for a naive pivot. */
  for (int i = 0; i < N; i++) v[i] = i;
  fs_cmp_calls = 0;
  obs_qsort(v, N, sizeof(v[0]), fs_cmp_int);
  for (int i = 0; i < N; i++) ASSERT_EQ(v[i], i);
  ASSERT_TRUE(fs_cmp_calls < N * N / 4); /* n log n is ~2500 here; n^2 is 90000 */

  /* Reversed, and all equal - the other two shapes that break a bad pivot. */
  for (int i = 0; i < N; i++) v[i] = N - 1 - i;
  obs_qsort(v, N, sizeof(v[0]), fs_cmp_int);
  for (int i = 0; i < N; i++) ASSERT_EQ(v[i], i);
  for (int i = 0; i < N; i++) v[i] = 7;
  fs_cmp_calls = 0;
  obs_qsort(v, N, sizeof(v[0]), fs_cmp_int);
  for (int i = 0; i < N; i++) ASSERT_EQ(v[i], 7);
  ASSERT_TRUE(fs_cmp_calls < N * N / 4);

  /* Two values only - the other classic killer, and the one that a partition which walks over
   * equals turns into n^2 even with a median-of-three pivot. */
  for (int i = 0; i < N; i++) v[i] = (i & 1) ? 1 : 0;
  fs_cmp_calls = 0;
  obs_qsort(v, N, sizeof(v[0]), fs_cmp_int);
  for (int i = 0; i < N; i++) ASSERT_EQ(v[i], (i < N / 2) ? 0 : 1);
  ASSERT_TRUE(fs_cmp_calls < N * N / 4);

  /* A scrambled one, and one below the insertion sort's threshold. */
  for (int i = 0; i < N; i++) v[i] = (i * 37 + 11) % N;
  obs_qsort(v, N, sizeof(v[0]), fs_cmp_int);
  for (int i = 0; i < N; i++) ASSERT_EQ(v[i], i);
  {
    int few[5] = {4, 2, 5, 1, 3};
    obs_qsort(few, 5, sizeof(few[0]), fs_cmp_int);
    for (int i = 0; i < 5; i++) ASSERT_EQ(few[i], i + 1);
  }
  /* Degenerate counts do nothing rather than reading past the end. */
  obs_qsort(v, 0, sizeof(v[0]), fs_cmp_int);
  obs_qsort(v, 1, sizeof(v[0]), fs_cmp_int);
  obs_qsort(NULL, 4, sizeof(v[0]), fs_cmp_int);

  /* An element wider than a word, to check the byte swap moves whole elements. */
  {
    struct pair { int key; int tag; };
    struct pair p[4] = {{3, 30}, {1, 10}, {4, 40}, {2, 20}};
    obs_qsort(p, 4, sizeof(p[0]), fs_cmp_int); /* the key is the first member */
    for (int i = 0; i < 4; i++) ASSERT_EQ(p[i].tag, p[i].key * 10);
  }

  for (int i = 0; i < N; i++) v[i] = i * 2;
  for (int i = 0; i < N; i++) {
    const int key = i * 2;
    const int *hit = (const int *)obs_bsearch(&key, v, N, sizeof(v[0]), fs_cmp_int);
    ASSERT_TRUE(hit != NULL);
    ASSERT_EQ(*hit, key);
  }
  {
    const int miss = 1; /* odd, so in no slot */
    ASSERT_TRUE(obs_bsearch(&miss, v, N, sizeof(v[0]), fs_cmp_int) == NULL);
    ASSERT_TRUE(obs_bsearch(&miss, v, 0, sizeof(v[0]), fs_cmp_int) == NULL);
  }
}

/*
 * `obs_sscanf`, which is `<libc/stdio.h>`'s `sscanf`.
 *
 * **The cases are the ones an asset loader meets**, because that is what it is for: an OBJ
 * vertex line, a face line with its slashes, an MTL colour, a key-value line, and the two
 * answers a read loop turns on - how many fields were assigned, and `EOF` when there was
 * nothing left to read at all.
 */
static void test_freestd_sscanf(void) {
  /* An OBJ vertex line, which is the whole reason this exists. */
  {
    float x = 0.0f, y = 0.0f, z = 0.0f;
    char tag[8] = {0};
    const int n = obs_sscanf("v  1.5 -2.25 3e2", "%7s %f %f %f", tag, &x, &y, &z);
    ASSERT_EQ(n, 4);
    ASSERT_STR_EQ(tag, "v");
    ASSERT_FLOAT_NEAR(x, 1.5f, 1e-6f);
    ASSERT_FLOAT_NEAR(y, -2.25f, 1e-6f);
    ASSERT_FLOAT_NEAR(z, 300.0f, 1e-3f);
  }
  /* A face line: the literal slashes in the format have to match, and do not skip whitespace. */
  {
    int v = 0, t = 0, nn = 0;
    const int n = obs_sscanf("7/12/3", "%d/%d/%d", &v, &t, &nn);
    ASSERT_EQ(n, 3);
    ASSERT_EQ(v, 7);
    ASSERT_EQ(t, 12);
    ASSERT_EQ(nn, 3);
  }
  /* A face line with no texture index - "7//3" - stops at the second slash, and the count says
   * so rather than leaving the caller to guess. */
  {
    int v = 0, t = -1, nn = -1;
    const int n = obs_sscanf("7//3", "%d/%d/%d", &v, &t, &nn);
    ASSERT_EQ(n, 1);
    ASSERT_EQ(v, 7);
    ASSERT_EQ(t, -1); /* untouched */
  }
  /* Whitespace in the format matches a run of it, including none. */
  {
    int a = 0, b = 0;
    ASSERT_EQ(obs_sscanf("  4\t\n 5", " %d %d", &a, &b), 2);
    ASSERT_EQ(a, 4);
    ASSERT_EQ(b, 5);
    ASSERT_EQ(obs_sscanf("4,5", "%d,%d", &a, &b), 2);
  }
  /* Widths, suppression and %n. */
  {
    int a = 0, b = 0, pos = 0;
    ASSERT_EQ(obs_sscanf("12345", "%2d%3d", &a, &b), 2);
    ASSERT_EQ(a, 12);
    ASSERT_EQ(b, 345);
    ASSERT_EQ(obs_sscanf("9 8 7", "%*d %d%n", &a, &pos), 1); /* %n does not count */
    ASSERT_EQ(a, 8);
    ASSERT_EQ(pos, 3);
  }
  /* The bases, and %i's own detection. */
  {
    unsigned int h = 0, o = 0;
    int i1 = 0, i2 = 0;
    ASSERT_EQ(obs_sscanf("ff 17", "%x %o", &h, &o), 2);
    ASSERT_EQ((int)h, 255);
    ASSERT_EQ((int)o, 15);
    ASSERT_EQ(obs_sscanf("0x10 010", "%i %i", &i1, &i2), 2);
    ASSERT_EQ(i1, 16);
    ASSERT_EQ(i2, 8);
  }
  /* The length modifiers reach the right width, which a wrong one corrupts silently. */
  {
    short s = 0;
    long l = 0;
    long long ll = 0;
    double d = 0.0;
    ASSERT_EQ(obs_sscanf("-5 -6 -7 1.5", "%hd %ld %lld %lf", &s, &l, &ll, &d), 4);
    ASSERT_EQ((int)s, -5);
    ASSERT_EQ((int)l, -6);
    ASSERT_EQ((int)ll, -7);
    ASSERT_TRUE(d > 1.49 && d < 1.51);
  }
  /* %c takes characters and does not terminate them; %[ ] is a set. */
  {
    char c[4] = {'z', 'z', 'z', 'z'};
    char key[16] = {0};
    char val[16] = {0};
    ASSERT_EQ(obs_sscanf("ab", "%2c", c), 1);
    ASSERT_EQ(c[0], 'a');
    ASSERT_EQ(c[1], 'b');
    ASSERT_EQ(c[2], 'z'); /* no terminator written */
    ASSERT_EQ(obs_sscanf("name = value", "%[^= ] = %[^\n]", key, val), 2);
    ASSERT_STR_EQ(key, "name");
    ASSERT_STR_EQ(val, "value");
  }
  /* **"1e" is a matching failure, not the number one.** C takes the longest sequence that could
   * begin a valid number - "1e" can, since "1e5" is one - and then fails when that sequence is
   * not itself valid. "1e5" is fine; "1e" converts nothing. */
  {
    float f = -1.0f;
    ASSERT_EQ(obs_sscanf("1e", "%f", &f), 0);
    ASSERT_FLOAT_NEAR(f, -1.0f, 1e-6f); /* untouched */
    ASSERT_EQ(obs_sscanf("1e5", "%f", &f), 1);
    ASSERT_FLOAT_NEAR(f, 100000.0f, 1e-1f);
  }
  /*
   * **The two failures a read loop tells apart.** A *matching* failure - the input is there and
   * is not a number - gives 0. An *input* failure, nothing left to read at all, gives EOF. C
   * draws that line and a loader turns on it: 0 means "skip this line", EOF means "stop".
   */
  {
    int a = 0;
    ASSERT_EQ(obs_sscanf("# a comment", "%d", &a), 0); /* a '#' is not a digit: no match */
    ASSERT_EQ(obs_sscanf("x1", "%d", &a), 0);
    ASSERT_EQ(obs_sscanf("", "%d", &a), -1);     /* nothing at all: end of input */
    ASSERT_EQ(obs_sscanf("   ", "%d", &a), -1);  /* and whitespace alone is nothing at all */
    ASSERT_EQ(obs_sscanf("1 x", "%d %d", &a, &a), 1); /* one field, then a matching failure */
    ASSERT_EQ(a, 1);
  }
}

/*
 * **The host's own `sscanf` as the oracle.**
 *
 * The cases above say what this should do; this says it does what C's does. The tests build on
 * the host, where `<stdio.h>` is the real library, so the same input and format go to both and
 * the return value and the converted values are compared. That is a stronger check than any
 * list of expectations, because it catches the rules nobody remembered to write a case for -
 * the first draft returned EOF where C returns 0, which is the difference between a loader
 * skipping a comment line and a loader stopping at one.
 */
static void test_freestd_sscanf_agrees_with_the_host(void) {
  static const char *const inputs[] = {
      "1 2 3",      "  -4  +5 ",  "7/12/3",      "7//3",       "x1",
      "",           "   ",        "1e",          "1e5",        "1.5e-3 2",
      "0x10 010",   "ff 17",      "12345",       "# comment",  "3.0",
      "1.",         ".5",         "-.25e1",      "  9",        "a b c",
      "+",          "-",          ".",           "0",          "00",
      "\t7\t8\t",   "0x",         "0xg",         "1e+",        "1e-5x",
      "-0",         "2147483648", "1.7976931348623157e308",    "1e400",
  };
  for (size_t i = 0; i < sizeof(inputs) / sizeof(inputs[0]); i++) {
    const char *const in = inputs[i];
    {
      int a = -999, b = -999, ha = -999, hb = -999;
      const int mine = obs_sscanf(in, "%d %d", &a, &b);
      const int theirs = sscanf(in, "%d %d", &ha, &hb);
      ASSERT_EQ(mine, theirs);
      if (theirs >= 1) ASSERT_EQ(a, ha);
      if (theirs >= 2) ASSERT_EQ(b, hb);
    }
    {
      float a = -999.0f, b = -999.0f, ha = -999.0f, hb = -999.0f;
      const int mine = obs_sscanf(in, "%f %f", &a, &b);
      const int theirs = sscanf(in, "%f %f", &ha, &hb);
      ASSERT_EQ(mine, theirs);
      if (theirs >= 1) ASSERT_FLOAT_NEAR(a, ha, 1e-5f);
      if (theirs >= 2) ASSERT_FLOAT_NEAR(b, hb, 1e-5f);
    }
    {
      int a = -999, b = -999, c = -999, ha = -999, hb = -999, hc = -999;
      const int mine = obs_sscanf(in, "%d/%d/%d", &a, &b, &c);
      const int theirs = sscanf(in, "%d/%d/%d", &ha, &hb, &hc);
      ASSERT_EQ(mine, theirs);
      if (theirs >= 1) ASSERT_EQ(a, ha);
    }
    {
      char a[32], ha[32];
      memset(a, 0, sizeof(a));
      memset(ha, 0, sizeof(ha));
      const int mine = obs_sscanf(in, "%31s", a);
      const int theirs = sscanf(in, "%31s", ha);
      ASSERT_EQ(mine, theirs);
      if (theirs >= 1) ASSERT_STR_EQ(a, ha);
    }
    {
      unsigned int a = 0u, ha = 0u;
      int b = -999, hb = -999;
      const int mine = obs_sscanf(in, "%x %i", &a, &b);
      const int theirs = sscanf(in, "%x %i", &ha, &hb);
      ASSERT_EQ(mine, theirs);
      if (theirs >= 1) ASSERT_EQ((int)a, (int)ha);
      if (theirs >= 2) ASSERT_EQ(b, hb);
    }
  }
}

void run_unit_tests_freestd(void) {
  TEST_SUITE_BEGIN("Freestanding Runtime Helpers");
  RUN_TEST(test_freestd_strings);
  RUN_TEST(test_freestd_formatting);
  RUN_TEST(test_freestd_nid);
  RUN_TEST(test_freestd_snprintf);
  RUN_TEST(test_freestd_sets_and_tokens);
  RUN_TEST(test_freestd_qsort_and_bsearch);
  RUN_TEST(test_freestd_sscanf);
  RUN_TEST(test_freestd_sscanf_agrees_with_the_host);
}
