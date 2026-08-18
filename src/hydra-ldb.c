/* hydra-ldb.c — read games from Hydra Launcher's LevelDB .ldb table.
 *
 * Format (verified against 000208.ldb):
 *  - File ends with 8-byte magic 0xdb4775248b80fb57 (LE).
 *  - 40 bytes before magic = footer: [varint mi_off, varint mi_size,
 *    varint idx_off, varint idx_size].
 *  - Each block (metaindex/index/data) = compressed data + 5-byte trailer
 *    [type:1][crc32c:4] located AFTER the block (at off+size). type 0x01 =
 *    snappy, 0x00 = uncompressed.
 *  - Decompressed block: entries, then restart array (N*4-byte LE offsets),
 *    then 4-byte LE count.
 *  - Entry: [varint shared][varint non_shared][varint value_len][key delta][value].
 *  - Data-block keys carry an 8-byte internal suffix (RocksDB-style); strip it.
 *  - Game records live under keys starting with "!games!"; the value is JSON.
 */

#include "hydra-ldb.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define HYDRA_LDB_MAGIC UINT64_C(0xdb4775248b80fb57)

#define GAME_TITLE_FIELD   "\"title\""
#define GAME_OID_FIELD     "\"objectId\""
#define GAME_SHOP_FIELD    "\"shop\""
#define GAME_EXEC_FIELD    "\"selectedDiscPath\""
#define GAME_ICON_FIELD    "\"iconUrl\""
#define GAME_DELETED_FIELD "\"isDeleted\""

#define GAME_NS "!games!"

/* ---- byte helpers ---- */

static uint8_t rd_u8 (const uint8_t *b, size_t n, size_t pos, int *ok)
{
  if (pos >= n) { *ok = 0; return 0; }
  return b[pos];
}

static uint32_t rd_u32le (const uint8_t *b, size_t n, size_t pos, int *ok)
{
  if (pos + 4 > n) { *ok = 0; return 0; }
  return (uint32_t)b[pos] | ((uint32_t)b[pos+1] << 8)
       | ((uint32_t)b[pos+2] << 16) | ((uint32_t)b[pos+3] << 24);
}

static uint64_t rd_varint (const uint8_t *b, size_t n, size_t *pos, int *ok)
{
  uint64_t val = 0;
  int shift = 0;
  for (int i = 0; i < 10; i++)
    {
      if (*pos >= n) { *ok = 0; return 0; }
      uint8_t c = b[(*pos)++];
      val |= (uint64_t)(c & 0x7f) << shift;
      if (!(c & 0x80)) return val;
      shift += 7;
    }
  *ok = 0;
  return 0;
}

/* ---- snappy decompressor ---- */

static int snappy_decompress (const uint8_t *in, size_t in_len,
                              uint8_t **out, size_t *out_len)
{
  size_t pos = 0;
  int ok = 1;
  uint64_t exp = rd_varint (in, in_len, &pos, &ok);
  if (!ok || exp > SIZE_MAX) return 0;

  uint8_t *o = malloc (exp ? (size_t)exp : 1);
  if (!o) return 0;
  size_t len = 0;

  while (pos < in_len)
    {
      uint8_t tag = rd_u8 (in, in_len, pos++, &ok);
      if (!ok) goto fail;
      unsigned typ = tag & 0x03;
      if (typ == 0) /* literal */
        {
          uint64_t l = (tag >> 2) & 0x3f;
          if (l >= 60)
            {
              unsigned extra = (unsigned)(l - 59);
              uint64_t v = 0;
              for (unsigned i = 0; i < extra; i++)
                {
                  uint8_t c = rd_u8 (in, in_len, pos++, &ok);
                  if (!ok) goto fail;
                  v |= (uint64_t)c << (8 * i);
                }
              l = v;
            }
          l += 1;
          if (pos + l > in_len || len + l > (size_t)exp) goto fail;
          memcpy (o + len, in + pos, (size_t)l);
          pos += (size_t)l;
          len += (size_t)l;
        }
      else
        {
          uint64_t l, off;
          if (typ == 1) /* copy1: length 4..11, 11-bit offset */
            {
              l = ((tag >> 2) & 0x07) + 4;
              uint8_t lo = rd_u8 (in, in_len, pos++, &ok);
              if (!ok) goto fail;
              off = ((tag >> 5) << 8) | lo;
            }
          else if (typ == 2) /* copy2: 16-bit offset */
            {
              l = (tag >> 2) + 1;
              if (pos + 2 > in_len) goto fail;
              off = in[pos] | (in[pos+1] << 8);
              pos += 2;
            }
          else /* copy4: 32-bit offset */
            {
              l = (tag >> 2) + 1;
              if (pos + 4 > in_len) goto fail;
              off = (uint64_t)in[pos] | ((uint64_t)in[pos+1] << 8)
                  | ((uint64_t)in[pos+2] << 16) | ((uint64_t)in[pos+3] << 24);
              pos += 4;
            }
          if (off == 0 || off > len || len + l > (size_t)exp) goto fail;
          size_t src = len - (size_t)off;
          for (uint64_t i = 0; i < l; i++)
            o[len + i] = o[src + i];
          len += (size_t)l;
        }
    }

  if (len != (size_t)exp) goto fail;
  *out = o;
  *out_len = len;
  return 1;

fail:
  free (o);
  return 0;
}

/* ---- LevelDB block parsing ---- */

typedef void (*EntryCb) (void *ud, const uint8_t *key, size_t keylen,
                         const uint8_t *val, size_t vallen);

static void block_parse (const uint8_t *dec, size_t n, EntryCb cb, void *ud)
{
  int ok = 1;
  if (n < 8) return;
  uint32_t cnt = rd_u32le (dec, n, n - 4, &ok);
  if (!ok) return;
  if (cnt > (n / 4)) return;
  size_t ra = n - 4 - (size_t)cnt * 4; /* start of restart array */

  uint8_t *prev = NULL;
  size_t prev_len = 0;
  size_t pos = 0;
  while (pos < ra)
    {
      size_t p = pos;
      uint64_t sh = rd_varint (dec, n, &p, &ok);
      if (!ok) break;
      uint64_t ns = rd_varint (dec, n, &p, &ok);
      if (!ok) break;
      uint64_t vl = rd_varint (dec, n, &p, &ok);
      if (!ok) break;
      if (sh > prev_len || ns > (ra - p) || vl > (ra - p - ns)) break;

      size_t keylen = (size_t)sh + (size_t)ns;
      uint8_t *key = malloc (keylen ? keylen : 1);
      if (!key) break;
      if (sh) memcpy (key, prev, (size_t)sh);
      memcpy (key + sh, dec + p, (size_t)ns);

      cb (ud, key, keylen, dec + p + ns, (size_t)vl);
      free (prev);
      prev = key;
      prev_len = keylen;
      pos = p + ns + vl;
    }
  free (prev);
}

static uint8_t *block_decompress (const uint8_t *data, size_t n,
                                  size_t off, size_t size,
                                  size_t *out_len)
{
  if (off + size + 5 > n) return NULL;
  uint8_t typ = data[off + size]; /* trailer: [type][crc32c] after block */
  if (typ == 0x00)
    {
      uint8_t *o = malloc (size ? size : 1);
      if (!o) return NULL;
      memcpy (o, data + off, size);
      *out_len = size;
      return o;
    }
  if (typ == 0x01)
    {
      uint8_t *o = NULL;
      size_t l = 0;
      if (snappy_decompress (data + off, size, &o, &l)) { *out_len = l; return o; }
      return NULL;
    }
  return NULL;
}

/* ---- index -> data block handles ---- */

typedef struct {
  size_t off;
  size_t size;
} BlockHandle;

/* We need to count handles, so use a small wrapper struct. */
typedef struct {
  BlockHandle *items;
  size_t count;
  size_t cap;
} HandleList;

static void handle_collect_list (void *ud, const uint8_t *key, size_t keylen,
                                 const uint8_t *val, size_t vallen)
{
  (void)key; (void)keylen;
  HandleList *hl = ud;
  size_t p = 0;
  int ok = 1;
  uint64_t off = rd_varint (val, vallen, &p, &ok);
  uint64_t size = ok ? rd_varint (val, vallen, &p, &ok) : 0;
  if (!ok || size == 0) return;
  if (hl->count == hl->cap)
    {
      hl->cap = hl->cap ? hl->cap * 2 : 16;
      hl->items = realloc (hl->items, hl->cap * sizeof (BlockHandle));
      if (!hl->items) return;
    }
  hl->items[hl->count].off = (size_t)off;
  hl->items[hl->count].size = (size_t)size;
  hl->count++;
}

/* ---- JSON string field extraction ---- */

static void json_field_copy (const uint8_t *val, size_t vlen,
                             const char *field, char *buf, size_t buflen)
{
  buf[0] = '\0';
  size_t flen = strlen (field);
  for (size_t i = 0; i + flen <= vlen; i++)
    {
      if (memcmp (val + i, field, flen) != 0) continue;
      size_t j = i + flen;
      while (j < vlen && (val[j] == ' ' || val[j] == ':')) j++;
      if (j >= vlen || val[j] != '"') return;
      j++;
      size_t k = 0;
      while (j < vlen && val[j] != '"')
        {
          if (val[j] == '\\' && j + 1 < vlen) j++; /* skip escape, keep next */
          if (k + 1 < buflen) buf[k++] = (char)val[j];
          j++;
        }
      buf[k] = '\0';
      return;
    }
}

/* Returns 1 if the JSON boolean field is present and truthy, 0 otherwise. */
static int json_bool_field (const uint8_t *val, size_t vlen, const char *field)
{
  size_t flen = strlen (field);
  for (size_t i = 0; i + flen <= vlen; i++)
    {
      if (memcmp (val + i, field, flen) != 0) continue;
      size_t j = i + flen;
      while (j < vlen && (val[j] == ' ' || val[j] == ':')) j++;
      while (j < vlen && (val[j] == ' ')) j++;
      if (j + 5 <= vlen && strncmp ((const char *)val + j, "true", 4) == 0
          && (j + 4 >= vlen || val[j+4] == ',' || val[j+4] == '}' || val[j+4] == ' '))
        return 1;
      if (j < vlen && val[j] == '1')
        return 1;
      return 0;
    }
  return 0;
}

/* ---- game extraction ---- */

typedef struct {
  HydraGameCb emit;
  void *ud;
  int n_games;
} GameCtx;

static void game_emit (void *ud, const uint8_t *key, size_t keylen,
                       const uint8_t *val, size_t vallen)
{
  GameCtx *gc = ud;
  /* strip 8-byte internal suffix: last 5 bytes are zero */
  size_t uk_len = keylen;
  if (keylen > 8 && key[keylen-5] == 0 && key[keylen-4] == 0
      && key[keylen-3] == 0 && key[keylen-2] == 0 && key[keylen-1] == 0)
    uk_len = keylen - 8;
  (void)uk_len;

  const size_t ns_len = sizeof (GAME_NS) - 1;
  if (keylen < ns_len + 8 || memcmp (key, GAME_NS, ns_len) != 0) return;

  char title[512], oid[128], shop[64], execp[2048], icon[2048];
  json_field_copy (val, vallen, GAME_TITLE_FIELD, title, sizeof title);
  json_field_copy (val, vallen, GAME_OID_FIELD, oid, sizeof oid);
  json_field_copy (val, vallen, GAME_SHOP_FIELD, shop, sizeof shop);
  json_field_copy (val, vallen, GAME_EXEC_FIELD, execp, sizeof execp);
  json_field_copy (val, vallen, GAME_ICON_FIELD, icon, sizeof icon);
  if (!title[0] || !oid[0]) return;
  /* Skip games the user deleted/hid in Hydra */
  if (json_bool_field (val, vallen, GAME_DELETED_FIELD)) return;

  HydraGame g;
  g.title = title;
  g.object_id = oid;
  g.shop = shop;
  g.exec_path = execp;
  g.icon_url = icon;
  gc->emit (&g, gc->ud);
  gc->n_games++;
}

int
hydra_ldb_parse (const uint8_t *data, size_t n,
                 HydraGameCb cb, void *user_data)
{
  if (!data || n < 48 + 8) return 0;
  uint64_t magic = UINT64_C(0);
  for (int i = 0; i < 8; i++)
    magic |= (uint64_t)data[n - 8 + i] << (8 * i);
  if (magic != HYDRA_LDB_MAGIC) return 0;

  int ok = 1;
  size_t f = n - 8 - 40;
  size_t p = f;
  rd_varint (data, n, &p, &ok); /* mi_off */
  rd_varint (data, n, &p, &ok); /* mi_size */
  uint64_t idx_off = ok ? rd_varint (data, n, &p, &ok) : 0;
  uint64_t idx_size = ok ? rd_varint (data, n, &p, &ok) : 0;
  if (!ok || idx_size == 0 || idx_off >= n) return 0;

  uint8_t *idx_dec = NULL;
  size_t idx_len = 0;
  idx_dec = block_decompress (data, n, (size_t)idx_off, (size_t)idx_size, &idx_len);
  if (!idx_dec) return 0;

  HandleList hl = { 0 };
  block_parse (idx_dec, idx_len, handle_collect_list, &hl);
  free (idx_dec);

  GameCtx gc = { .emit = cb, .ud = user_data, .n_games = 0 };
  for (size_t i = 0; i < hl.count; i++)
    {
      uint8_t *dec = NULL;
      size_t dlen = 0;
      dec = block_decompress (data, n, hl.items[i].off, hl.items[i].size, &dlen);
      if (dec)
        {
          block_parse (dec, dlen, game_emit, &gc);
          free (dec);
        }
    }
  free (hl.items);
  return gc.n_games;
}
