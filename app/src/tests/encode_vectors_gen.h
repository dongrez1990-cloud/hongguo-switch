/*
 * GENERATED FILE -- do not edit by hand.
 *
 * URL-encoding vectors, produced by running the reference
 * pythonUrlEncode() (N3urda/hongguoTV server/vendor/hongguo.cjs:79,
 * GPL-3.0). Ground truth is the reference implementation.
 */

#ifndef ENCODE_VECTORS_GEN_H
#define ENCODE_VECTORS_GEN_H

#define ENC_COUNT 20

typedef struct { const char *name; const char *input; const char *expected; } EncVector;

/* For the map case, `input` is a placeholder; the expected string is compared
 * against hg_url_encode_map() driven by the same fixed key/value list that
 * gen_encode_vectors.mjs used. */
#define ENC_MAP_INDEX 19
static const EncVector ENC_VECTORS[ENC_COUNT] = {
  {
    "plain",
    "abc",
    "q=abc",
  },
  {
    "digit",
    "12345",
    "q=12345",
  },
  {
    "unreserved-dash-underscore-dot-tilde",
    "a-b_c.d~e",
    "q=a-b_c.d~e",
  },
  {
    "STAR is kept by encodeURIComponent (Python would escape it)",
    "a*b",
    "q=a*b",
  },
  {
    "bang-quote-paren are escaped explicitly",
    "a!b'c(d)e",
    "q=a%21b%27c%28d%29e",
  },
  {
    "space",
    "a b",
    "q=a%20b",
  },
  {
    "plus",
    "a+b",
    "q=a%2Bb",
  },
  {
    "slash and colon",
    "a/b:c",
    "q=a%2Fb%3Ac",
  },
  {
    "ampersand and equals",
    "a&b=c",
    "q=a%26b%3Dc",
  },
  {
    "percent",
    "a%b",
    "q=a%25b",
  },
  {
    "question and hash",
    "a?b#c",
    "q=a%3Fb%23c",
  },
  {
    "comma semicolon at",
    "a,b;c@d",
    "q=a%2Cb%3Bc%40d",
  },
  {
    "brackets and braces",
    "a[b]c{d}",
    "q=a%5Bb%5Dc%7Bd%7D",
  },
  {
    "backslash and caret",
    "a\\b^c",
    "q=a%5Cb%5Ec",
  },
  {
    "chinese",
    "\347\237\255\345\211\247",
    "q=%E7%9F%AD%E5%89%A7",
  },
  {
    "chinese with space",
    "\347\272\242\346\236\234 \347\237\255\345\211\247",
    "q=%E7%BA%A2%E6%9E%9C%20%E7%9F%AD%E5%89%A7",
  },
  {
    "emoji",
    "\360\237\216\254",
    "q=%F0%9F%8E%AC",
  },
  {
    "key and value both need encoding",
    "k=1",
    "q=k%3D1",
  },
  {
    "empty",
    "",
    "q=",
  },
  {
    "device map (ordering + joining)",
    "<map>",
    "iid=1905892595382586&device_id=1905892595378490&ac=wifi&aid=8662&device_type=25053RT47C&language=zh&resolution=1280*2772&version_name=7.1.3.32&ts=1800600000&_rticket=1800600000123",
  },
};

#endif /* ENCODE_VECTORS_GEN_H */
