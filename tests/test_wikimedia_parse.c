// botmanager — MIT
// Cases for the wikimedia parse layer: the one place in that plugin
// where being wrong is silent. A statement that keeps its deprecated
// rank, a date padded past its precision, or an unknown value collapsed
// into a missing one all reach a channel — and a corpus — as a fact
// nobody flagged, spelled correctly.
//
// Every row drives a distinct code path rather than a distinct input:
// rank filtering, dedup, qualifier order, each snak datatype, each date
// precision, and the sitelink re-rank that decides which candidate won.
#include "test.h"

#define WIKIMEDIA_INTERNAL
#include "wikimedia.h"

#define OUT_SZ 512

// ----------------------------------------------------------------------
// Flatten a parsed result so one string comparison covers the whole of
// it. A value is <kind letter>:<text>, a quantity carries its unit in
// brackets, and a statement's qualifiers follow it in parentheses.
// ----------------------------------------------------------------------

static char
kind_letter(wm_value_kind_t k)
{
  switch(k)
  {
    case WM_VAL_UNKNOWN:  return('U');
    case WM_VAL_NONE:     return('N');
    case WM_VAL_ITEM:     return('I');
    case WM_VAL_TIME:     return('T');
    case WM_VAL_QUANTITY: return('Q');
    case WM_VAL_COORD:    return('C');
    case WM_VAL_TEXT:     return('S');
  }

  return('?');
}

static void
render(const wm_claim_t *claims, uint8_t n, char *out, size_t cap)
{
  size_t pos = 0;

  out[0] = '\0';

  for(uint8_t i = 0; i < n; i++)
  {
    const wm_claim_t *c = &claims[i];
    int               need;

    need = snprintf(out + pos, cap - pos, "%s%c:%s", i > 0 ? ";" : "",
        kind_letter(c->value.kind), c->value.text);

    if(need < 0 || (size_t)need >= cap - pos)
      return;

    pos += (size_t)need;

    if(c->value.kind == WM_VAL_QUANTITY && c->value.unit[0] != '\0')
    {
      need = snprintf(out + pos, cap - pos, "[%s]", c->value.unit);

      if(need < 0 || (size_t)need >= cap - pos)
        return;

      pos += (size_t)need;
    }

    for(uint8_t q = 0; q < c->n_quals; q++)
    {
      need = snprintf(out + pos, cap - pos, "%s%s%s%s %c:%s%s",
          q == 0 ? "(" : ",", c->quals[q].property,
          c->quals[q].label[0] != '\0' ? "=" : "", c->quals[q].label,
          kind_letter(c->quals[q].value.kind), c->quals[q].value.text,
          q + 1 == c->n_quals ? ")" : "");

      if(need < 0 || (size_t)need >= cap - pos)
        return;

      pos += (size_t)need;
    }
  }
}

// ----------------------------------------------------------------------
// Fixtures. Shapes are the live ones (probed 2026-08-24), trimmed to the
// fields the parser reads.
// ----------------------------------------------------------------------

#define ITEM(id) \
  "{\"snaktype\":\"value\",\"property\":\"P527\",\"datavalue\":{\"value\":" \
  "{\"entity-type\":\"item\",\"id\":\"" id "\"},\"type\":" \
  "\"wikibase-entityid\"},\"datatype\":\"wikibase-item\"}"

#define TIME_SNAK(stamp, prec) \
  "{\"snaktype\":\"value\",\"property\":\"P580\",\"datavalue\":{\"value\":" \
  "{\"time\":\"" stamp "\",\"precision\":" prec "},\"type\":\"time\"}," \
  "\"datatype\":\"time\"}"

// Rank, dedup and qualifiers in one payload: a preferred statement that
// must lead, the same item stated twice, a deprecated statement that
// must vanish, and a start-time qualifier at year precision.
static const char rank_fx[] =
  "{\"claims\":{\"P527\":["
    "{\"mainsnak\":" ITEM("Q1") ",\"rank\":\"normal\","
      "\"qualifiers\":{\"P580\":[" TIME_SNAK("+1982-00-00T00:00:00Z", "9")
      "]},\"qualifiers-order\":[\"P580\"]},"
    "{\"mainsnak\":" ITEM("Q1") ",\"rank\":\"normal\"},"
    "{\"mainsnak\":" ITEM("Q2") ",\"rank\":\"deprecated\"},"
    "{\"mainsnak\":" ITEM("Q3") ",\"rank\":\"preferred\"}"
  "]}}";

// One statement per snak datatype the plugin claims to understand,
// including the two that say "no value" in different ways.
static const char kinds_fx[] =
  "{\"claims\":{\"P1\":["
    "{\"mainsnak\":{\"snaktype\":\"novalue\",\"property\":\"P1\"},"
      "\"rank\":\"normal\"},"
    "{\"mainsnak\":{\"snaktype\":\"somevalue\",\"property\":\"P1\"},"
      "\"rank\":\"normal\"},"
    "{\"mainsnak\":{\"snaktype\":\"value\",\"property\":\"P1\",\"datavalue\":"
      "{\"value\":\"hello\",\"type\":\"string\"},\"datatype\":\"string\"},"
      "\"rank\":\"normal\"},"
    "{\"mainsnak\":{\"snaktype\":\"value\",\"property\":\"P1\",\"datavalue\":"
      "{\"value\":{\"text\":\"bonjour\",\"language\":\"fr\"},\"type\":"
      "\"monolingualtext\"},\"datatype\":\"monolingualtext\"},"
      "\"rank\":\"normal\"},"
    "{\"mainsnak\":{\"snaktype\":\"value\",\"property\":\"P1\",\"datavalue\":"
      "{\"value\":{\"amount\":\"+1.88\",\"unit\":"
      "\"http://www.wikidata.org/entity/Q11573\"},\"type\":\"quantity\"},"
      "\"datatype\":\"quantity\"},\"rank\":\"normal\"},"
    "{\"mainsnak\":{\"snaktype\":\"value\",\"property\":\"P1\",\"datavalue\":"
      "{\"value\":{\"amount\":\"+42\",\"unit\":\"1\"},\"type\":\"quantity\"},"
      "\"datatype\":\"quantity\"},\"rank\":\"normal\"},"
    "{\"mainsnak\":{\"snaktype\":\"value\",\"property\":\"P1\",\"datavalue\":"
      "{\"value\":{\"latitude\":48.8567,\"longitude\":2.3508},\"type\":"
      "\"globecoordinate\"},\"datatype\":\"globe-coordinate\"},"
      "\"rank\":\"normal\"}"
  "]}}";

// Every precision the renderer distinguishes, plus a BCE date, whose
// sign is the reason the year is not simply the first four bytes.
static const char time_fx[] =
  "{\"claims\":{\"P2\":["
    "{\"mainsnak\":{\"snaktype\":\"value\",\"property\":\"P2\",\"datavalue\":"
      "{\"value\":{\"time\":\"+1879-03-14T00:00:00Z\",\"precision\":11},"
      "\"type\":\"time\"},\"datatype\":\"time\"},\"rank\":\"normal\"},"
    "{\"mainsnak\":{\"snaktype\":\"value\",\"property\":\"P2\",\"datavalue\":"
      "{\"value\":{\"time\":\"+0415-03-00T00:00:00Z\",\"precision\":10},"
      "\"type\":\"time\"},\"datatype\":\"time\"},\"rank\":\"normal\"},"
    "{\"mainsnak\":{\"snaktype\":\"value\",\"property\":\"P2\",\"datavalue\":"
      "{\"value\":{\"time\":\"+1982-00-00T00:00:00Z\",\"precision\":9},"
      "\"type\":\"time\"},\"datatype\":\"time\"},\"rank\":\"normal\"},"
    "{\"mainsnak\":{\"snaktype\":\"value\",\"property\":\"P2\",\"datavalue\":"
      "{\"value\":{\"time\":\"-0044-03-15T00:00:00Z\",\"precision\":11},"
      "\"type\":\"time\"},\"datatype\":\"time\"},\"rank\":\"normal\"}"
  "]}}";

// The batched lookup's answer: labels for an item value, a quantity's
// unit and a qualifier property — and one id it does not carry, which
// must survive as itself rather than as an empty cell.
// An item as wbgetclaims hands it over: a curated property, an
// identifier, a pointer into Wikimedia's own plumbing, and one the
// menu below does not name at all.
static const char facts_fx[] =
  "{\"claims\":{"
    "\"P2013\":[{\"mainsnak\":{\"snaktype\":\"value\",\"property\":"
      "\"P2013\",\"datavalue\":{\"value\":\"greenday\",\"type\":"
      "\"string\"},\"datatype\":\"external-id\"},\"rank\":\"normal\"},"
    "{\"mainsnak\":{\"snaktype\":\"value\",\"property\":\"P2013\","
      "\"datavalue\":{\"value\":\"gd\",\"type\":\"string\"},"
      "\"datatype\":\"external-id\"},\"rank\":\"normal\"}],"
    "\"P910\":[{\"mainsnak\":" ITEM("Q9") ",\"rank\":\"normal\"}],"
    "\"P527\":[{\"mainsnak\":" ITEM("Q1") ",\"rank\":\"normal\"},"
      "{\"mainsnak\":" ITEM("Q3") ",\"rank\":\"normal\"}],"
    "\"P571\":[{\"mainsnak\":" TIME_SNAK("+1982-00-00T00:00:00Z", "9")
      ",\"rank\":\"normal\"}]"
  "}}";

// Labels for everything the block above will want to name.
static const char facts_labels_fx[] =
  "{\"entities\":{"
    "\"P527\":{\"labels\":{\"en\":{\"value\":\"has part\"}},"
      "\"datatype\":\"wikibase-item\"},"
    "\"P571\":{\"labels\":{\"en\":{\"value\":\"inception\"}},"
      "\"datatype\":\"time\"},"
    "\"P910\":{\"labels\":{\"en\":{\"value\":\"main category\"}},"
      "\"datatype\":\"wikibase-item\"},"
    "\"P2013\":{\"labels\":{\"en\":{\"value\":\"Facebook id\"}},"
      "\"datatype\":\"external-id\"},"
    "\"Q1\":{\"labels\":{\"en\":{\"value\":\"Billie\"}}},"
    "\"Q3\":{\"labels\":{\"en\":{\"value\":\"Mike\"}}},"
    "\"Q9\":{\"labels\":{\"en\":{\"value\":\"Category:Green Day\"}}},"
    "\"Q47871\":{\"labels\":{\"en\":{\"value\":\"Green Day\"}},"
      "\"descriptions\":{\"en\":{\"value\":\"American rock band\"}}}"
  "}}";

static const char labels_fx[] =
  "{\"entities\":{"
    "\"P527\":{\"id\":\"P527\",\"datatype\":\"wikibase-item\",\"labels\":"
      "{\"en\":{\"language\":\"en\",\"value\":\"has part\"}}},"
    "\"Q1\":{\"id\":\"Q1\",\"labels\":{\"en\":{\"language\":\"en\","
      "\"value\":\"Billie Joe Armstrong\"}}},"
    "\"P580\":{\"id\":\"P580\",\"labels\":{\"en\":{\"language\":\"en\","
      "\"value\":\"start time\"}}}"
  "}}";

static const struct
{
  const char *name;
  const char *doc;
  const char *property;
  const char *want;
} claim_cases[] = {
  { "preferred leads, duplicates collapse, deprecated vanishes",
    rank_fx, "P527", "I:Q3;I:Q1(P580 T:1982)" },
  { "a property nobody stated parses to nothing", rank_fx, "P31", "" },
  { "every snak datatype", kinds_fx, "P1",
    "N:;U:;S:hello;S:bonjour;Q:1.88[Q11573];Q:42;C:48.85670, 2.35080" },
  { "a date is never padded past its precision", time_fx, "P2",
    "T:1879-03-14;T:0415-03;T:1982;T:-0044-03-15" },
};

static const struct
{
  const char *name;
  const char *in;
  const char *want;
} normalize_cases[] = {
  { "already normal", "green day", "green day" },
  { "case and padding", "  Green  DAY ", "green day" },
  { "tabs and newlines are whitespace", "date\tof\nbirth", "date of birth" },
  { "empty", "   ", "" },
};

static const struct
{
  const char *name;
  const char *in;
  const char *want;
} encode_cases[] = {
  { "unreserved passes through", "Green_Day-2.0~x", "Green_Day-2.0~x" },
  { "space", "green day", "green%20day" },
  { "the separator that must not arrive raw", "a|b", "a%7Cb" },
  { "utf-8 is encoded byte by byte", "é", "%C3%A9" },
};

static const struct
{
  const char *name;
  const char *in;
  char        kind;
  bool        want;
  const char *norm;
} qid_cases[] = {
  { "an item", "Q47871", 'Q', true, "Q47871" },
  { "a property", "P569", 'P', true, "P569" },
  { "typed in lower case", "q47871", 'Q', true, "Q47871" },
  { "wrong series", "P569", 'Q', false, "" },
  { "a bare letter is not an id", "Q", 'Q', false, "" },
  { "an article title is not an id", "Green Day", 'Q', false, "" },
  { "digits alone are not an id", "47871", 'Q', false, "" },
  { "an id longer than the field is refused",
    "Q123456789012345678", 'Q', false, "" },
};

int
main(void)
{
  char out[OUT_SZ];

  for(size_t i = 0; i < sizeof(claim_cases) / sizeof(claim_cases[0]); i++)
  {
    struct json_object *root;
    wm_claim_t          claims[WM_CLAIMS_MAX];
    uint8_t             n;

    root = json_parse_buf(claim_cases[i].doc, strlen(claim_cases[i].doc),
        "test");

    if(root == NULL)
      return(test_skip("wikimedia_parse", "fixture would not parse"));

    n = wm_claims_parse(json_get_obj(root, "claims"), claim_cases[i].property,
        claims, WM_CLAIMS_MAX);

    render(claims, n, out, sizeof(out));
    test_check_str("claims", claim_cases[i].name, claim_cases[i].want, out);
    json_object_put(root);
  }

  // The batched lookup turns ids into words in place. What it cannot
  // reach — Q3 here — keeps its id, which is unlovely but never wrong.
  {
    struct json_object *doc;
    struct json_object *ents;
    wm_claims_res_t     res;
    char                ids[WM_IDS_MAX][WM_QID_SZ];
    char                joined[256];
    uint8_t             n_ids;

    memset(&res, 0, sizeof(res));
    strlcpy(res.entity, "Q47871", sizeof(res.entity));
    strlcpy(res.property, "P527", sizeof(res.property));

    doc  = json_parse_buf(rank_fx, sizeof(rank_fx) - 1, "test");
    ents = json_parse_buf(labels_fx, sizeof(labels_fx) - 1, "test");

    if(doc == NULL || ents == NULL)
      return(test_skip("wikimedia_parse", "fixture would not parse"));

    res.n = wm_claims_parse(json_get_obj(doc, "claims"), "P527", res.claims,
        WM_CLAIMS_MAX);

    // The property has no label yet, so it rides the same batch as the
    // values — that is what `extra` is for.
    n_ids = wm_ids_collect(&res, res.property, ids, WM_IDS_MAX);
    test_check_sz("labels", "one id per distinct item, unit and qualifier",
        4, n_ids);

    test_check_bool("labels", "the batch joins for an ids= parameter",
        SUCCESS, wm_ids_join(ids, n_ids, joined, sizeof(joined)));
    test_check_str("labels", "ids arrive pipe-separated and encoded",
        "P527%7CQ3%7CQ1%7CP580", joined);

    wm_labels_apply(json_get_obj(ents, "entities"), &res, "en");

    test_check_str("labels", "the answering property gets its label",
        "has part", res.label);
    test_check_str("labels", "and its datatype", "wikibase-item",
        res.datatype);

    render(res.claims, res.n, out, sizeof(out));
    test_check_str("labels", "ids become words, and what is missing stays",
        "I:Q3;I:Billie Joe Armstrong(P580=start time T:1982)", out);

    json_object_put(doc);
    json_object_put(ents);
  }

  // Prominence is the ranking key and the sort is stable, so two items
  // of equal prominence keep the order the searchers gave them.
  {
    wm_candidate_t hits[4];
    size_t         pos = 0;

    memset(hits, 0, sizeof(hits));
    strlcpy(hits[0].qid, "PSG", WM_QID_SZ);   hits[0].sitelinks = 102;
    strlcpy(hits[1].qid, "Paris", WM_QID_SZ); hits[1].sitelinks = 366;
    strlcpy(hits[2].qid, "tie-a", WM_QID_SZ); hits[2].sitelinks = 12;
    strlcpy(hits[3].qid, "tie-b", WM_QID_SZ); hits[3].sitelinks = 12;

    wm_rank_window(hits, 4);
    out[0] = '\0';

    for(uint8_t i = 0; i < 4; i++)
      pos += (size_t)snprintf(out + pos, sizeof(out) - pos, "%s%s",
          i > 0 ? "," : "", hits[i].qid);

    test_check_str("rank", "sitelinks decide, ties keep their order",
        "Paris,PSG,tie-a,tie-b", out);
  }

  for(size_t i = 0;
      i < sizeof(normalize_cases) / sizeof(normalize_cases[0]); i++)
  {
    wm_normalize(normalize_cases[i].in, out, sizeof(out));
    test_check_str("normalize", normalize_cases[i].name,
        normalize_cases[i].want, out);
  }

  for(size_t i = 0; i < sizeof(encode_cases) / sizeof(encode_cases[0]); i++)
  {
    size_t need = wm_urlencode(encode_cases[i].in, out, sizeof(out));

    test_check_str("urlencode", encode_cases[i].name, encode_cases[i].want,
        out);

    // The return is the promise every call site sizes its buffer
    // against — truncation is `need >= cap`.
    test_check_sz("urlencode length", encode_cases[i].name,
        strlen(encode_cases[i].want), need);
  }

  for(size_t i = 0; i < sizeof(qid_cases) / sizeof(qid_cases[0]); i++)
  {
    test_check_bool("is_qid", qid_cases[i].name, qid_cases[i].want,
        wm_is_qid(qid_cases[i].in, qid_cases[i].kind));

    if(!qid_cases[i].want)
      continue;

    wm_qid_norm(qid_cases[i].in, out, WM_QID_SZ);
    test_check_str("qid_norm", qid_cases[i].name, qid_cases[i].norm, out);
  }

  // The fact block: which properties a card carries, in what order, and
  // which of them are not answers at all.
  {
    struct json_object *doc;
    struct json_object *ents;
    wm_facts_res_t      res;
    wm_menu_res_t       menu;
    char                ids[WM_IDS_MAX][WM_QID_SZ];
    uint8_t             n_ids;

    doc  = json_parse_buf(facts_fx, sizeof(facts_fx) - 1, "test");
    ents = json_parse_buf(facts_labels_fx, sizeof(facts_labels_fx) - 1,
        "test");

    if(doc == NULL || ents == NULL)
      return(test_skip("wikimedia_parse", "fixture would not parse"));

    // A menu names three properties; the item has statements for two of
    // them, and the third simply does not appear.
    memset(&menu, 0, sizeof(menu));
    strlcpy(menu.props[0].property, "P571", WM_QID_SZ);
    strlcpy(menu.props[0].label, "inception", WM_LABEL_SZ);
    strlcpy(menu.props[1].property, "P2044", WM_QID_SZ);
    strlcpy(menu.props[1].label, "elevation", WM_LABEL_SZ);
    strlcpy(menu.props[2].property, "P527", WM_QID_SZ);
    strlcpy(menu.props[2].label, "has part", WM_LABEL_SZ);
    menu.n = 3;

    memset(&res, 0, sizeof(res));
    strlcpy(res.qid, "Q47871", sizeof(res.qid));
    res.n = wm_facts_parse(json_get_obj(doc, "claims"), &menu, res.facts,
        WM_FACTS_MAX);

    test_check_sz("facts", "menu order, and only what has a value",
        2, res.n);
    test_check_str("facts", "the menu's order, not the item's",
        "inception", res.facts[0].label);
    test_check_str("facts", "and the second it named", "has part",
        res.facts[1].label);

    // The statement-ordered fall-back keeps only what a card can say:
    // the identifiers and the Facebook handle are strings and go.
    memset(&res, 0, sizeof(res));
    strlcpy(res.qid, "Q47871", sizeof(res.qid));
    res.n = wm_facts_parse_any(json_get_obj(doc, "claims"), res.facts,
        WM_FACTS_MAX);

    test_check_sz("facts", "no menu: every non-string property survives",
        3, res.n);

    n_ids = wm_fact_ids_collect(&res, ids, WM_IDS_MAX);
    test_check_str("facts", "the item leads the label batch", "Q47871",
        ids[0]);
    test_check_bool("facts", "an unlabelled property rides it too", true,
        n_ids >= 4);

    wm_fact_labels_apply(json_get_obj(ents, "entities"), &res, "en");

    test_check_str("facts", "the item's own label rides the batch",
        "Green Day", res.label);
    test_check_str("facts", "and its description", "American rock band",
        res.description);
    test_check_sz("facts", "the category pointer goes once it can be read",
        2, res.n);
    test_check_str("facts", "and the order the item stated survives it",
        "has part", res.facts[0].label);
    test_check_str("facts", "with its ids turned into words", "Billie",
        res.facts[0].values[0].text);

    json_object_put(doc);
    json_object_put(ents);
  }

  return(test_report("wikimedia_parse"));
}
