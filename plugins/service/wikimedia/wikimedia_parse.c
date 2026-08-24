// botmanager — MIT
// Wikimedia's wire formats turned into this plugin's types: snaks,
// statement rank, date precision, the label batch, and the sitelink
// re-rank that decides which candidate won.
//
// Everything here is a pure function of its arguments — no KV, no curl,
// no module state — which is what lets tests/test_wikimedia_parse.c
// drive it directly. The provider (wikimedia.c) owns the wire.
#define WIKIMEDIA_INTERNAL
#include "wikimedia.h"

// True for a Wikidata id of the given series — 'Q' for an item, 'P' for
// a property. This is the boundary every caller-supplied identifier
// crosses: past it, an id is pasted into a URL unescaped.
bool
wm_is_qid(const char *s, char kind)
{
  size_t n;

  if(s == NULL || (s[0] != kind && s[0] != (char)tolower((unsigned char)kind)))
    return(false);

  for(n = 1; s[n] != '\0'; n++)
  {
    if(s[n] < '0' || s[n] > '9')
      return(false);
  }

  return(n > 1 && n < WM_QID_SZ);
}


// Copy a caller-supplied id with its series letter upcased. Wikidata's
// own ids are uppercase and the API is not forgiving about it, so "q42"
// typed by a user is answered rather than refused.
void
wm_qid_norm(const char *in, char *out, size_t cap)
{
  strlcpy(out, in, cap);
  out[0] = (char)toupper((unsigned char)out[0]);
}


// Lowercase, trim, and collapse interior runs of whitespace. Cache keys
// and property words both go through this so "Green  Day " and
// "green day" are one query rather than two.
void
wm_normalize(const char *in, char *out, size_t cap)
{
  size_t n = 0;
  bool   gap = false;

  if(cap == 0)
    return;

  out[0] = '\0';

  if(in == NULL)
    return;

  for(const unsigned char *p = (const unsigned char *)in; *p != '\0'; p++)
  {
    if(isspace(*p))
    {
      gap = n > 0;
      continue;
    }

    if(gap && n + 1 < cap)
      out[n++] = ' ';

    gap = false;

    if(n + 1 >= cap)
      break;

    out[n++] = (char)tolower(*p);
  }

  out[n] = '\0';
}


// Percent-encode `in` into `out`, RFC-3986 unreserved set kept verbatim.
// Returns the number of bytes the full encoding needs (may exceed cap,
// like snprintf).
size_t
wm_urlencode(const char *in, char *out, size_t cap)
{
  static const char hex[] = "0123456789ABCDEF";
  size_t n = 0;

  if(cap == 0)
    return(0);

  for(const unsigned char *p = (const unsigned char *)in; *p != '\0'; p++)
  {
    unsigned char c = *p;
    bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
                      || (c >= '0' && c <= '9')
                      || c == '-' || c == '_' || c == '.' || c == '~';

    if(unreserved)
    {
      if(n + 1 < cap)
        out[n] = (char)c;

      n++;
      continue;
    }

    if(n + 3 < cap)
    {
      out[n]     = '%';
      out[n + 1] = hex[c >> 4];
      out[n + 2] = hex[c & 0x0f];
    }

    n += 3;
  }

  out[n < cap ? n : cap - 1] = '\0';
  return(n);
}


// Render a Wikidata time value at its declared precision and never
// beyond it. "+0415-03-00T00:00:00Z" at precision 10 is March 415; the
// day is not zero, it is absent, and padding it to the 1st invents a
// fact. The year keeps its sign, so BCE survives.
void
wm_time_render(struct json_object *val, wm_time_t *out, char *text,
    size_t cap)
{
  struct json_object *stamp = NULL;
  const char         *s;
  const char         *dash;
  int32_t             prec = 0;
  size_t              len;
  size_t              keep;
  size_t              year;

  text[0] = '\0';

  if(val == NULL || !json_object_object_get_ex(val, "time", &stamp))
    return;

  s = json_object_get_string(stamp);

  if(s == NULL || s[0] == '\0')
    return;

  json_get_int(val, "precision", &prec);
  out->precision = (uint8_t)prec;

  // "±YYYY-MM-DD…". Every CE date is stamped with a leading '+' that
  // nobody wants to read; a BCE one keeps its '-', which is also why
  // the year is not simply the first four bytes.
  if(s[0] == '+')
    s++;

  len  = strlen(s);
  dash = strchr(s[0] == '-' ? s + 1 : s, '-');
  year = dash != NULL ? (size_t)(dash - s) : len;

  keep = prec >= 11 ? year + 6   // year + "-MM-DD"
       : prec == 10 ? year + 3   // year + "-MM"
       :              year;

  if(keep > len)
    keep = len;

  if(keep >= cap)
    keep = cap - 1;

  memcpy(text, s, keep);
  text[keep] = '\0';

  strlcpy(out->text, text, sizeof(out->text));
}


// Read the trailing entity id out of a Wikidata concept URI
// ("http://www.wikidata.org/entity/Q4917"). A unitless quantity carries
// the literal "1" instead, which is not an id and yields "".
void
wm_uri_qid(const char *uri, char *out, size_t cap)
{
  const char *tail;

  out[0] = '\0';

  if(uri == NULL)
    return;

  tail = strrchr(uri, '/');
  tail = tail != NULL ? tail + 1 : uri;

  if(wm_is_qid(tail, 'Q'))
    strlcpy(out, tail, cap);
}


// One snak -> one value. The three no-answer shapes stay distinct:
// novalue is "recorded as none", somevalue is "recorded as unknown", and
// a snak we cannot read at all is also unknown — never a fact.
void
wm_snak_parse(struct json_object *snak, wm_value_t *out)
{
  struct json_object *dv  = NULL;
  struct json_object *val = NULL;
  char                snaktype[16];
  char                type[32];
  const char         *raw;

  memset(out, 0, sizeof(*out));
  out->kind = WM_VAL_UNKNOWN;

  if(snak == NULL)
    return;

  snaktype[0] = '\0';
  json_get_str(snak, "snaktype", snaktype, sizeof(snaktype));

  if(strcmp(snaktype, "novalue") == 0)
  {
    out->kind = WM_VAL_NONE;
    return;
  }

  if(strcmp(snaktype, "value") != 0)
    return;

  dv = json_get_obj(snak, "datavalue");

  if(dv == NULL || !json_object_object_get_ex(dv, "value", &val))
    return;

  type[0] = '\0';
  json_get_str(dv, "type", type, sizeof(type));

  if(strcmp(type, "wikibase-entityid") == 0)
  {
    out->kind = WM_VAL_ITEM;
    json_get_str(val, "id", out->qid, sizeof(out->qid));

    // Until the batched label lookup lands, an item is its own id — and
    // if that lookup cannot reach it, the id is what a consumer prints.
    strlcpy(out->text, out->qid, sizeof(out->text));
    return;
  }

  if(strcmp(type, "time") == 0)
  {
    out->kind = WM_VAL_TIME;
    wm_time_render(val, &out->time, out->text, sizeof(out->text));
    return;
  }

  if(strcmp(type, "quantity") == 0)
  {
    char amount[64];
    char unit[256];

    out->kind = WM_VAL_QUANTITY;
    amount[0] = '\0';
    unit[0]   = '\0';
    json_get_str(val, "amount", amount, sizeof(amount));
    json_get_str(val, "unit", unit, sizeof(unit));

    // The amount arrives as an exact decimal string ("+11500000000",
    // "+1.88"); it is rendered verbatim rather than through a float, and
    // parsed alongside only for consumers that want to compute.
    out->amount = strtod(amount, NULL);
    strlcpy(out->text, amount[0] == '+' ? amount + 1 : amount,
        sizeof(out->text));

    // Provisionally the unit's id, replaced by its label if the lookup
    // reaches it — the same convention an item's text follows.
    wm_uri_qid(unit, out->unit, sizeof(out->unit));
    return;
  }

  if(strcmp(type, "globecoordinate") == 0)
  {
    out->kind = WM_VAL_COORD;
    json_get_double(val, "latitude", &out->lat);
    json_get_double(val, "longitude", &out->lon);
    snprintf(out->text, sizeof(out->text), "%.5f, %.5f", out->lat, out->lon);
    return;
  }

  out->kind = WM_VAL_TEXT;

  if(strcmp(type, "monolingualtext") == 0)
  {
    json_get_str(val, "text", out->text, sizeof(out->text));
    return;
  }

  raw = json_object_get_string(val);

  if(raw != NULL)
    strlcpy(out->text, raw, sizeof(out->text));
}


// Two values name the same thing when they name the same item, or when
// they read the same. Wikidata repeats a property across statements that
// differ only in their references, and a consumer must not print the
// same member four times (Wittgenstein's mother does exactly that).
bool
wm_value_same(const wm_value_t *a, const wm_value_t *b)
{
  if(a->kind != b->kind)
    return(false);

  if(a->kind == WM_VAL_ITEM)
    return(strcmp(a->qid, b->qid) == 0);

  return(strcmp(a->text, b->text) == 0);
}


// Pull the qualifiers of one statement, in the order Wikidata declares
// them. Only the first snak of each qualifier property is carried: the
// rest are refinements of a refinement.
uint8_t
wm_quals_parse(struct json_object *st, wm_qualifier_t *out, uint8_t cap)
{
  struct json_object *quals;
  struct json_object *order;
  uint8_t             n = 0;
  int                 len;

  quals = json_get_obj(st, "qualifiers");
  order = json_get_array(st, "qualifiers-order");

  if(quals == NULL || order == NULL)
    return(0);

  len = (int)json_object_array_length(order);

  for(int i = 0; i < len && n < cap; i++)
  {
    struct json_object *pid = json_object_array_get_idx(order, i);
    struct json_object *arr = NULL;
    const char         *prop;

    if(pid == NULL)
      continue;

    prop = json_object_get_string(pid);

    if(prop == NULL || !json_object_object_get_ex(quals, prop, &arr))
      continue;

    if(!json_object_is_type(arr, json_type_array)
        || json_object_array_length(arr) == 0)
      continue;

    strlcpy(out[n].property, prop, sizeof(out[n].property));
    out[n].label[0] = '\0';
    wm_snak_parse(json_object_array_get_idx(arr, 0), &out[n].value);
    n++;
  }

  return(n);
}


// Every statement Wikidata holds for one property on one item, filtered
// and ordered the way a reader expects: deprecated dropped, preferred
// ahead of normal, duplicates collapsed.
uint8_t
wm_claims_parse(struct json_object *claims, const char *property,
    wm_claim_t *out, uint8_t cap)
{
  struct json_object *arr;
  uint8_t             n = 0;
  int                 len;

  if(claims == NULL)
    return(0);

  arr = json_get_array(claims, property);

  if(arr == NULL)
    return(0);

  len = (int)json_object_array_length(arr);

  // Two passes rather than a sort: rank has exactly two surviving
  // values, and the second pass is what makes the first one's entries
  // the dedup baseline.
  for(int pass = 0; pass < 2 && n < cap; pass++)
  {
    for(int i = 0; i < len && n < cap; i++)
    {
      struct json_object *st = json_object_array_get_idx(arr, i);
      wm_claim_t          c;
      char                rank[16];
      bool                dup = false;

      if(st == NULL)
        continue;

      rank[0] = '\0';
      json_get_str(st, "rank", rank, sizeof(rank));

      if(strcmp(rank, "deprecated") == 0)
        continue;

      if((strcmp(rank, "preferred") == 0) != (pass == 0))
        continue;

      memset(&c, 0, sizeof(c));
      c.preferred = pass == 0;
      wm_snak_parse(json_get_obj(st, "mainsnak"), &c.value);

      for(uint8_t j = 0; j < n && !dup; j++)
        dup = wm_value_same(&out[j].value, &c.value);

      if(dup)
        continue;

      c.n_quals = wm_quals_parse(st, c.quals, WM_QUALS_MAX);
      out[n++]  = c;
    }
  }

  return(n);
}


// Append `id` to a bounded set, ignoring blanks and duplicates.
void
wm_id_push(char (*ids)[WM_QID_SZ], uint8_t *n, uint8_t cap, const char *id)
{
  if(id == NULL || id[0] == '\0' || *n >= cap)
    return;

  for(uint8_t i = 0; i < *n; i++)
  {
    if(strcmp(ids[i], id) == 0)
      return;
  }

  strlcpy(ids[*n], id, WM_QID_SZ);
  (*n)++;
}


// Everything in a claims result that is currently an id and wants to be
// a word: item values, quantity units, qualifier properties and their
// item values — plus the answering property itself when it arrived as a
// P-id and so has no label yet.
uint8_t
wm_ids_collect(const wm_claims_res_t *res, const char *extra,
    char (*out)[WM_QID_SZ], uint8_t cap)
{
  uint8_t n = 0;

  wm_id_push(out, &n, cap, extra);

  for(uint8_t i = 0; i < res->n; i++)
  {
    const wm_claim_t *c = &res->claims[i];

    if(c->value.kind == WM_VAL_ITEM)
      wm_id_push(out, &n, cap, c->value.qid);

    if(c->value.kind == WM_VAL_QUANTITY)
      wm_id_push(out, &n, cap, c->value.unit);

    for(uint8_t q = 0; q < c->n_quals; q++)
    {
      wm_id_push(out, &n, cap, c->quals[q].property);

      if(c->quals[q].value.kind == WM_VAL_ITEM)
        wm_id_push(out, &n, cap, c->quals[q].value.qid);
    }
  }

  return(n);
}


// entities[<id>].labels.<lang>.value, or NULL where the lookup did not
// reach it.
const char *
wm_label_of(struct json_object *entities, const char *id, const char *lang)
{
  struct json_object *ent = NULL;
  struct json_object *labels;
  struct json_object *loc;
  struct json_object *val = NULL;

  if(entities == NULL || id == NULL || id[0] == '\0')
    return(NULL);

  if(!json_object_object_get_ex(entities, id, &ent))
    return(NULL);

  labels = json_get_obj(ent, "labels");
  loc    = labels != NULL ? json_get_obj(labels, lang) : NULL;

  if(loc == NULL || !json_object_object_get_ex(loc, "value", &val))
    return(NULL);

  return(json_object_get_string(val));
}


// Replace every id that has a label with its label, and give a quantity
// its unit. What the batch could not reach keeps its id, which is
// unlovely but never wrong.
void
wm_labels_apply(struct json_object *entities, wm_claims_res_t *res,
    const char *lang)
{
  const char *lbl;

  if(res->label[0] == '\0')
  {
    struct json_object *prop = NULL;

    lbl = wm_label_of(entities, res->property, lang);

    if(lbl != NULL)
      strlcpy(res->label, lbl, sizeof(res->label));

    if(res->datatype[0] == '\0'
        && json_object_object_get_ex(entities, res->property, &prop))
      json_get_str(prop, "datatype", res->datatype, sizeof(res->datatype));
  }

  for(uint8_t i = 0; i < res->n; i++)
  {
    wm_claim_t *c = &res->claims[i];

    if(c->value.kind == WM_VAL_ITEM)
    {
      lbl = wm_label_of(entities, c->value.qid, lang);

      if(lbl != NULL)
        strlcpy(c->value.text, lbl, sizeof(c->value.text));
    }

    if(c->value.kind == WM_VAL_QUANTITY && c->value.unit[0] != '\0')
    {
      lbl = wm_label_of(entities, c->value.unit, lang);

      if(lbl != NULL)
        strlcpy(c->value.unit, lbl, sizeof(c->value.unit));

      strlcat(c->value.text, " ", sizeof(c->value.text));
      strlcat(c->value.text, c->value.unit, sizeof(c->value.text));
    }

    for(uint8_t q = 0; q < c->n_quals; q++)
    {
      wm_qualifier_t *ql = &c->quals[q];

      lbl = wm_label_of(entities, ql->property, lang);

      if(lbl != NULL)
        strlcpy(ql->label, lbl, sizeof(ql->label));

      if(ql->value.kind != WM_VAL_ITEM)
        continue;

      lbl = wm_label_of(entities, ql->value.qid, lang);

      if(lbl != NULL)
        strlcpy(ql->value.text, lbl, sizeof(ql->value.text));
    }
  }
}


// Join an id set for an ids= parameter. The separator is a pipe, which
// has to arrive percent-encoded.
bool
wm_ids_join(char (*ids)[WM_QID_SZ], uint8_t n, char *out, size_t cap)
{
  size_t pos = 0;

  if(cap == 0)
    return(FAIL);

  out[0] = '\0';

  for(uint8_t i = 0; i < n; i++)
  {
    int need = snprintf(out + pos, cap - pos, "%s%s", i > 0 ? "%7C" : "",
        ids[i]);

    if(need < 0 || (size_t)need >= cap - pos)
      return(FAIL);

    pos += (size_t)need;
  }

  return(n > 0 ? SUCCESS : FAIL);
}

// Rank a resolved window by prominence. Sitelink count is the key, and
// it is what fixes both searchers at once: PSG (102) falls behind Paris
// (366), the colour gold (61) behind the element (277). Insertion sort,
// because ten entries is the whole population and stability is what
// preserves relevance order between items of equal prominence.
void
wm_rank_window(wm_candidate_t *hits, uint8_t n)
{
  for(uint8_t i = 1; i < n; i++)
  {
    wm_candidate_t tmp = hits[i];
    uint8_t        j   = i;

    while(j > 0 && hits[j - 1].sitelinks < tmp.sitelinks)
    {
      hits[j] = hits[j - 1];
      j--;
    }

    hits[j] = tmp;
  }
}
