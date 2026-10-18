/*
 * GENERATED FILE -- do not edit by hand.
 *
 * Branch-2 signing vectors, produced from N3urda/hongguoTV
 * server/vendor/hongguo.cjs (GPL-3.0). Ground truth is the reference
 * implementation, not an independent source -- see the note in
 * hg_sign_test.c about what that does and does not prove.
 */

#ifndef BRANCH2_VECTORS_GEN_H
#define BRANCH2_VECTORS_GEN_H

#define B2_COUNT 6

typedef struct {
  unsigned int ts;
  const char *url;
  const char *body;   /* NULL for a GET */
  unsigned int body_len;
  const char *hash;
} B2Vector;

static const B2Vector B2_VECTORS[B2_COUNT] = {
  {
    1800600000u,
    "https://api5-normal-sinfonlinea.fqnovel.com/reading/bookapi/search/tab/v?q=%E7%9F%AD%E5%89%A7&ticket=1",
    "{\"video\":\"9876543210\"}", 22u,
    "517f093655a84489dbfe1b569f15597d0e1b7049",
  },
  {
    1800600001u,
    "https://api5-normal-sinfonlinea.fqnovel.com/reading/bookapi/search/tab/v?q=%E7%9F%AD%E5%89%A7&ticket=6",
    0, 0u,
    "45ed90945a69fc98a29ef2a61698d212fec12d77",
  },
  {
    1800600002u,
    "https://api5-normal-sinfonlinea.fqnovel.com/reading/bookapi/search/tab/v?q=%E7%9F%AD%E5%89%A7&ticket=1",
    "{\"video\":\"9876543210\"}", 22u,
    "5a422cb0a8e536de7f0fa1363866baf18614c34c",
  },
  {
    1800600003u,
    "https://api5-normal-sinfonlinea.fqnovel.com/reading/bookapi/search/tab/v?q=%E7%9F%AD%E5%89%A7&ticket=1",
    0, 0u,
    "6ceb5de4bac97bd612e84076838c8524b5c9ecb4",
  },
  {
    1800600004u,
    "https://api5-normal-sinfonlinea.fqnovel.com/reading/bookapi/search/tab/v?q=%E7%9F%AD%E5%89%A7&ticket=6",
    "{\"video\":\"9876543210\"}", 22u,
    "ef39b5105fd5669838b05e02d7ec35307d1565dc",
  },
  {
    1800600005u,
    "https://api5-normal-sinfonlinea.fqnovel.com/reading/bookapi/search/tab/v?q=%E7%9F%AD%E5%89%A7&ticket=1",
    0, 0u,
    "c317e430553fbf52407305dba2804be0f4aa89da",
  },
};

#endif /* BRANCH2_VECTORS_GEN_H */
