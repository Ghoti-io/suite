/*
 * The RFC 8448 known answers, as a program, for suite/tools/xarch/tls.sh.
 *
 * Reads the two vector files tools/extract-rfc8448.py wrote from the pinned
 * RFC text, drives libs/tls's client through the simple 1-RTT trace and the
 * HelloRetryRequest trace (whole, and one byte at a time), compares every
 * secret, the second ClientHello and the client's Finished with the RFC's
 * values; seals and opens the RFC's records, handshake and application, in
 * both directions; drives the server with the RFC's ClientHello and checks its
 * flight, as a message and as records, against the RFC; and runs a real client
 * against a real server over a loopback with the stream split at several
 * sizes; checks RFC 8448's resumed handshake (the PSK a ticket stands for, the
 * binder, and every secret after them), rebuilds the RFC's NewSessionTicket and
 * ServerHello with its PSK, and runs a real client and server through a full
 * handshake, a ticket and a resumption; and RFC 8448's 0-RTT trace (the early
 * secret, key and IV, and the sealed early record and EndOfEarlyData) and a real
 * client and server resuming with early data: delivered once and read apart
 * from ordinary data, or, after a HelloRetryRequest, refused and handed back.
 * It prints one line per check. The output is the same on every target
 * or something is wrong with that target, which is what the script compares;
 * the exit status is non-zero if any check failed.
 *
 *   tls-probe DIR     DIR holds simple-1rtt.vec, hrr.vec and resumed.vec; DIR/../pki the
 *                     test credentials
 */

#include <ghoti.io/tls/tls.h>
#include <ghoti.io/security/security.h>

#include "client/client_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXV 400

typedef struct {
  char * key[MAXV];
  unsigned char * val[MAXV];
  size_t len[MAXV];
  int n;
} Vec;

static int failures = 0;

static void check(int ok, const char * what) {
  printf("%s %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) {
    failures++;
  }
}

static int hexval(int c) {
  return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

static int load(Vec * v, const char * dir, const char * name) {
  char path[1024];
  char * line = NULL;
  size_t cap = 0;
  FILE * f;

  snprintf(path, sizeof path, "%s/%s.vec", dir, name);
  f = fopen(path, "r");
  if (f == NULL) {
    fprintf(stderr, "cannot open %s\n", path);
    return 0;
  }
  memset(v, 0, sizeof *v);
  while (getline(&line, &cap, f) > 0) {
    char * sp;
    size_t n, i;

    if (line[0] == '#' || line[0] == '\n') {
      continue;
    }
    sp = strchr(line, ' ');
    if (sp == NULL || v->n >= MAXV) {
      continue;
    }
    *sp++ = 0;
    n = strlen(sp);
    while (n > 0 && (sp[n - 1] == '\n' || sp[n - 1] == '\r')) {
      sp[--n] = 0;
    }
    v->key[v->n] = strdup(line);
    v->val[v->n] = malloc(n / 2 + 1);
    for (i = 0; i + 1 < n; i += 2) {
      v->val[v->n][i / 2] = (unsigned char)(hexval(sp[i]) * 16 + hexval(sp[i + 1]));
    }
    v->len[v->n] = n / 2;
    v->n++;
  }
  free(line);
  fclose(f);
  return 1;
}

static const unsigned char * get(const Vec * v, const char * key, size_t * len) {
  int i;

  for (i = 0; i < v->n; i++) {
    if (strcmp(v->key[i], key) == 0) {
      *len = v->len[i];
      return v->val[i];
    }
  }
  fprintf(stderr, "no vector %s\n", key);
  failures++;
  *len = 0;
  return (const unsigned char *)"";
}

static int same(const Vec * v, const char * key, const void * got, size_t got_len) {
  size_t n;
  const unsigned char * want = get(v, key, &n);

  return n == got_len && n != 0 && memcmp(want, got, n) == 0;
}

typedef struct {
  const unsigned char * scalar;
  size_t len;
} Random;

static GTLS_Result rnd(void * user, void * out, size_t n) {
  Random * r = user;

  if (r->len != n) {
    return GTLS_ERR_IO;
  }
  memcpy(out, r->scalar, n);
  return GTLS_OK;
}

static GTLS_Result accept_chain(void * u, const GTLS_Conn * c,
    const GTLS_CertificateMsg * m, int64_t t) {
  (void)u;
  (void)c;
  (void)m;
  (void)t;
  return GTLS_OK;
}

static int feed(GTLS_Conn * c, GTLS_Epoch e, const unsigned char * p, size_t n, size_t chunk) {
  size_t at;

  if (chunk == 0) {
    return gtls_conn_feed(c, e, p, n, 1700000000) == GTLS_OK;
  }
  for (at = 0; at < n; at += chunk) {
    size_t m = n - at < chunk ? n - at : chunk;
    if (gtls_conn_feed(c, e, p + at, m, 1700000000) != GTLS_OK) {
      return 0;
    }
  }
  return 1;
}

static int pending_is(GTLS_Conn * c, GTLS_Epoch e, const unsigned char * want, size_t n) {
  const unsigned char * p = NULL;
  size_t len = 0;

  if (gtls_conn_pending_output(c, e, &p, &len) != GTLS_OK) {
    return 0;
  }
  return len == n && (n == 0 || memcmp(p, want, n) == 0);
}

static void drain(GTLS_Conn * c, GTLS_Epoch e) {
  const unsigned char * p = NULL;
  size_t len = 0;

  if (gtls_conn_pending_output(c, e, &p, &len) == GTLS_OK) {
    (void)gtls_conn_consume_output(c, e, len);
  }
}

static void trace(const char * dir, const char * file, int hrr, size_t chunk) {
  Vec v;
  GCERT_TrustStore * trust = NULL;
  GTLS_ContextConfig cfg;
  GTLS_Context * ctx = NULL;
  GTLS_Conn * c = NULL;
  Random r;
  char name[160];
  size_t n, m;
  const unsigned char * ch1;
  const unsigned char * x25519;
  const unsigned char * p256;
  const unsigned char * flight;
  unsigned char secret[GTLS_HASH_MAX];
  size_t slen = 0;

  if (!load(&v, dir, file)) {
    failures++;
    return;
  }
  ch1 = get(&v, "client.construct-a-clienthello-handshake-message.clienthello", &n);
  x25519 = get(&v, "client.create-an-ephemeral-x25519-key-pair.private-key", &m);
  p256 = hrr ? get(&v, "client.create-an-ephemeral-p-256-key-pair.private-key", &m) : x25519;
  flight = NULL;
  gcert_trust_new(NULL, NULL, &trust);
  gtls_context_config_default(&cfg);
  cfg.trust = trust;
  gtls_context_new(NULL, &cfg, &ctx);
  gtls_conn_new(ctx, "server", 6, &c);
  r.scalar = p256;
  r.len = 32;
  gtls_conn_set_hooks(c, rnd, &r, accept_chain, NULL);

  snprintf(name, sizeof name, "%s chunk %zu", file, chunk);
  check(gtls_conn_start_raw(c, ch1, n, x25519) == GTLS_OK, "start with the RFC's ClientHello");
  check(pending_is(c, GTLS_EPOCH_INITIAL, ch1, n), "the ClientHello goes out as the RFC has it");
  drain(c, GTLS_EPOCH_INITIAL);
  if (hrr) {
    const unsigned char * msg = get(&v, "server.construct-a-serverhello-handshake-message.serverhello", &n);
    check(feed(c, GTLS_EPOCH_INITIAL, msg, n, chunk), "HelloRetryRequest accepted");
    ch1 = get(&v, "client.construct-a-clienthello-handshake-message@2.clienthello", &n);
    check(pending_is(c, GTLS_EPOCH_INITIAL, ch1, n), "the second ClientHello is the RFC's, byte for byte");
    drain(c, GTLS_EPOCH_INITIAL);
    msg = get(&v, "server.construct-a-serverhello-handshake-message@2.serverhello", &n);
    check(feed(c, GTLS_EPOCH_INITIAL, msg, n, chunk), "ServerHello accepted");
    flight = get(&v, "server.send-handshake-record@3.payload", &n);
  } else {
    const unsigned char * msg = get(&v, "server.construct-a-serverhello-handshake-message.serverhello", &n);
    check(feed(c, GTLS_EPOCH_INITIAL, msg, n, chunk), "ServerHello accepted");
    flight = get(&v, "server.send-handshake-record@2.payload", &n);
  }
  gtls_conn_debug_secret(c, GTLS_SECRET_HANDSHAKE, secret, sizeof secret, &slen);
  check(same(&v, "server.extract-secret-handshake.secret", secret, slen), "handshake secret");
  gtls_conn_debug_secret(c, GTLS_SECRET_CLIENT_HS, secret, sizeof secret, &slen);
  check(same(&v, "server.derive-secret-tls13-c-hs-traffic.expanded", secret, slen), "client handshake traffic secret");
  gtls_conn_debug_secret(c, GTLS_SECRET_SERVER_HS, secret, sizeof secret, &slen);
  check(same(&v, "server.derive-secret-tls13-s-hs-traffic.expanded", secret, slen), "server handshake traffic secret");
  check(feed(c, GTLS_EPOCH_HANDSHAKE, flight, n, chunk), "EncryptedExtensions through Finished accepted");
  check(gtls_conn_is_connected(c), "connected");
  {
    const unsigned char * fin = get(&v, "client.construct-a-finished-handshake-message.finished", &m);
    check(pending_is(c, GTLS_EPOCH_HANDSHAKE, fin, m), "the client's Finished is the RFC's");
  }
  gtls_conn_debug_secret(c, GTLS_SECRET_MASTER, secret, sizeof secret, &slen);
  check(same(&v, "server.extract-secret-master.secret", secret, slen), "master secret");
  gtls_conn_debug_secret(c, GTLS_SECRET_CLIENT_AP, secret, sizeof secret, &slen);
  check(same(&v, "server.derive-secret-tls13-c-ap-traffic.expanded", secret, slen), "client application traffic secret");
  gtls_conn_debug_secret(c, GTLS_SECRET_SERVER_AP, secret, sizeof secret, &slen);
  check(same(&v, "server.derive-secret-tls13-s-ap-traffic.expanded", secret, slen), "server application traffic secret");
  {
    unsigned char key[GTLS_KEY_MAX], iv[GTLS_IV_LEN];
    gtls_traffic_keys(gtls_conn_suite(c), secret, key, iv);
    check(same(&v, "server.derive-write-traffic-keys-for-application-data.key-expanded", key, 16) &&
        same(&v, "server.derive-write-traffic-keys-for-application-data.iv-expanded", iv, 12),
        "application key and IV");
  }
  (void)name;
  gtls_conn_free(c);
  gtls_context_free(ctx);
  gcert_trust_free(trust);
}

/* ---------------------------------------------------------------- records */

static int open_expect(GTLS_RecordKeys * k, const Vec * v, const char * record, const char * payload,
    unsigned type) {
  size_t rn, pn;
  const unsigned char * rec = get(v, record, &rn);
  const unsigned char * want = get(v, payload, &pn);
  unsigned char out[GTLS_RECORD_CIPHER_MAX];
  size_t n = 0;
  unsigned t = 0;

  if (rn < 5 || gtls_record_open(k, rec, rec + 5, rn - 5, 64, out, &n, &t) != GTLS_OK) {
    return 0;
  }
  return n == pn && t == type && memcmp(out, want, n) == 0;
}

static int seal_expect(GTLS_RecordKeys * k, const Vec * v, const char * record, const char * payload,
    unsigned type) {
  size_t rn, pn;
  const unsigned char * rec = get(v, record, &rn);
  const unsigned char * pt = get(v, payload, &pn);
  GTLS_Buf b;
  int ok;

  gtls_buf_init(&b, NULL, 1u << 16);
  ok = gtls_record_seal(k, type, pt, pn, 0, &b) == GTLS_OK && b.len == rn && memcmp(b.data, rec, rn) == 0;
  gtls_buf_free(&b);
  return ok;
}

static GTLS_RecordKeys keys_of(const Vec * v, const char * prefix) {
  char name[200];
  GTLS_RecordKeys k;
  size_t n;
  const unsigned char * key;
  const unsigned char * iv;

  snprintf(name, sizeof name, "%s.key-expanded", prefix);
  key = get(v, name, &n);
  snprintf(name, sizeof name, "%s.iv-expanded", prefix);
  iv = get(v, name, &n);
  memset(&k, 0, sizeof k);
  (void)gtls_record_keys_from_raw(&k, GTLS_SUITE_AES_128_GCM_SHA256, key, iv, 0);
  return k;
}

static void records(const char * dir) {
  Vec v;
  GTLS_RecordKeys k;

  if (!load(&v, dir, "simple-1rtt")) {
    failures++;
    return;
  }
  k = keys_of(&v, "server.derive-write-traffic-keys-for-handshake-data");
  check(seal_expect(&k, &v, "server.send-handshake-record@2.complete-record",
            "server.send-handshake-record@2.payload", GTLS_CT_HANDSHAKE), "the server's handshake record, sealed");
  k = keys_of(&v, "server.derive-write-traffic-keys-for-handshake-data");
  check(open_expect(&k, &v, "server.send-handshake-record@2.complete-record",
            "server.send-handshake-record@2.payload", GTLS_CT_HANDSHAKE), "the server's handshake record, opened");
  k = keys_of(&v, "server.derive-read-traffic-keys-for-handshake-data");
  check(seal_expect(&k, &v, "client.send-handshake-record@2.complete-record",
            "client.send-handshake-record@2.payload", GTLS_CT_HANDSHAKE), "the client's Finished record, sealed");
  k = keys_of(&v, "client.derive-write-traffic-keys-for-application-data");
  check(seal_expect(&k, &v, "client.send-application-data-record.complete-record",
            "client.send-application-data-record.payload", GTLS_CT_APPLICATION_DATA), "the client's data record, sealed");
  check(seal_expect(&k, &v, "client.send-alert-record.complete-record", "client.send-alert-record.payload",
            GTLS_CT_ALERT), "the client's alert record, the second under its key");
  k = keys_of(&v, "server.derive-write-traffic-keys-for-application-data");
  check(seal_expect(&k, &v, "server.send-handshake-record@3.complete-record",
            "server.send-handshake-record@3.payload", GTLS_CT_HANDSHAKE), "the server's ticket record, sealed");
  check(seal_expect(&k, &v, "server.send-application-data-record.complete-record",
            "server.send-application-data-record.payload", GTLS_CT_APPLICATION_DATA), "the server's data record, sealed");
  check(seal_expect(&k, &v, "server.send-alert-record.complete-record", "server.send-alert-record.payload",
            GTLS_CT_ALERT), "the server's alert record, the third under its key");
  k = keys_of(&v, "server.derive-write-traffic-keys-for-application-data");
  check(open_expect(&k, &v, "server.send-handshake-record@3.complete-record",
            "server.send-handshake-record@3.payload", GTLS_CT_HANDSHAKE) &&
      open_expect(&k, &v, "server.send-application-data-record.complete-record",
            "server.send-application-data-record.payload", GTLS_CT_APPLICATION_DATA) &&
      open_expect(&k, &v, "server.send-alert-record.complete-record", "server.send-alert-record.payload",
            GTLS_CT_ALERT), "the server's three application records, opened in order");
}

/* ------------------------------------------------------------ the RFC server */

static unsigned char * slurp(const char * dir, const char * name, size_t * len) {
  char path[1024];
  FILE * f;
  unsigned char * buf;
  long n;

  snprintf(path, sizeof path, "%s/../pki/%s", dir, name);
  f = fopen(path, "rb");
  if (f == NULL) {
    fprintf(stderr, "cannot open %s\n", path);
    failures++;
    return NULL;
  }
  fseek(f, 0, SEEK_END);
  n = ftell(f);
  fseek(f, 0, SEEK_SET);
  buf = malloc((size_t)n);
  if (buf == NULL || fread(buf, 1, (size_t)n, f) != (size_t)n) {
    failures++;
  }
  fclose(f);
  *len = (size_t)n;
  return buf;
}

typedef struct {
  const unsigned char * chunk[2];
  size_t len[2];
  int draws;
} ServerRandom;

static GTLS_Result server_rnd(void * user, void * out, size_t n) {
  ServerRandom * r = user;
  int d = r->draws++;

  if (d > 1 || r->len[d] != n) {
    return GTLS_ERR_IO;
  }
  memcpy(out, r->chunk[d], n);
  return GTLS_OK;
}

typedef struct {
  GCERT_X509 leaf;
  const unsigned char * sig;
  size_t sig_len;
  int verified;
} RfcSigner;

static GTLS_Result rfc_sign(void * user, const GTLS_Conn * c, unsigned scheme, const unsigned char * content,
    size_t n, unsigned char * out, size_t cap, size_t * len) {
  RfcSigner * s = user;

  (void)c;
  s->verified = gcert_x509_verify_signature(&s->leaf, (GCERT_SigScheme)scheme, content, n, s->sig,
      s->sig_len) == GCERT_OK;
  if (s->sig_len > cap) {
    return GTLS_ERR_LIMIT;
  }
  memcpy(out, s->sig, s->sig_len);
  *len = s->sig_len;
  return GTLS_OK;
}

static int stream_is(GTLS_Conn * c, const unsigned char * a, size_t an, const unsigned char * b, size_t bn) {
  const unsigned char * p = NULL;
  size_t n = 0;
  int ok;

  if (gtls_conn_stream_output(c, &p, &n) != GTLS_OK) {
    return 0;
  }
  ok = n == an + bn && memcmp(p, a, an) == 0 && (bn == 0 || memcmp(p + an, b, bn) == 0);
  (void)gtls_conn_stream_consume(c, n);
  return ok;
}

static void rfc_server(const char * dir, int stream_mode, size_t chunk) {
  Vec v;
  size_t cert_len = 0, key_len = 0, n = 0, m = 0, der_len;
  unsigned char * ed_cert = slurp(dir, "ed-leaf.der", &cert_len);
  unsigned char * ed_key = slurp(dir, "ed-leaf.pkcs8", &key_len);
  GCERT_Key * key = NULL;
  GTLS_ContextConfig cfg;
  GTLS_Identity id;
  GTLS_Context * ctx = NULL;
  GTLS_Conn * c = NULL;
  const void * chain[1];
  size_t lens[1];
  const unsigned char * rfc_cert;
  const unsigned char * cv;
  const unsigned char * ee;
  const unsigned char * sh;
  const unsigned char * seam[1];
  size_t seam_len[1];
  ServerRandom rr;
  RfcSigner signer;
  const unsigned char * ch;
  unsigned char secret[GTLS_HASH_MAX];
  size_t slen = 0;

  if (ed_cert == NULL || ed_key == NULL || !load(&v, dir, "simple-1rtt")) {
    failures++;
    return;
  }
  gcert_key_from_pkcs8(NULL, ed_key, key_len, &key);
  gtls_context_config_default(&cfg);
  cfg.role = GTLS_ROLE_SERVER;
  chain[0] = ed_cert;
  lens[0] = cert_len;
  memset(&id, 0, sizeof id);
  id.chain = chain;
  id.chain_lens = lens;
  id.chain_count = 1;
  id.key = key;
  cfg.identities = &id;
  cfg.identity_count = 1;
  check(gtls_context_new(NULL, &cfg, &ctx) == GTLS_OK, "a server context from an identity");
  gtls_conn_new_server(ctx, &c);

  rfc_cert = get(&v, "server.construct-a-certificate-handshake-message.certificate", &n);
  der_len = ((size_t)rfc_cert[8] << 16) | ((size_t)rfc_cert[9] << 8) | rfc_cert[10];
  cv = get(&v, "server.construct-a-certificateverify-handshake-message.certificateverify", &m);
  ee = get(&v, "server.construct-an-encryptedextensions-handshake-message.encryptedextensions", &n);
  sh = get(&v, "server.construct-a-serverhello-handshake-message.serverhello", &m);
  memset(&signer, 0, sizeof signer);
  check(gcert_x509_parse(rfc_cert + 11, der_len, &signer.leaf) == GCERT_OK, "the RFC's certificate parses");
  signer.sig = cv + 8;
  signer.sig_len = ((size_t)cv[6] << 8) | cv[7];
  seam[0] = rfc_cert + 11;
  seam_len[0] = der_len;
  rr.chunk[0] = sh + 6;
  rr.len[0] = 32;
  rr.chunk[1] = get(&v, "server.create-an-ephemeral-x25519-key-pair.private-key", &m);
  rr.len[1] = 32;
  rr.draws = 0;
  gtls_conn_set_hooks(c, server_rnd, &rr, NULL, NULL);
  gtls_conn_set_server_hooks(c, rfc_sign, &signer, seam, seam_len, 1, 0x0804, ee + 6, n - 6 - 4);
  check(gtls_conn_start(c) == GTLS_OK, "the server starts");

  if (!stream_mode) {
    size_t at;

    ch = get(&v, "client.construct-a-clienthello-handshake-message.clienthello", &n);
    for (at = 0; at < n; at += chunk == 0 ? n : chunk) {
      size_t k = chunk == 0 ? n : (n - at < chunk ? n - at : chunk);
      if (gtls_conn_feed(c, GTLS_EPOCH_INITIAL, ch + at, k, 1700000000) != GTLS_OK) {
        failures++;
      }
    }
    sh = get(&v, "server.construct-a-serverhello-handshake-message.serverhello", &n);
    check(pending_is(c, GTLS_EPOCH_INITIAL, sh, n), "the server's ServerHello is the RFC's, byte for byte");
    {
      const unsigned char * fl = get(&v, "server.send-handshake-record@2.payload", &n);
      check(pending_is(c, GTLS_EPOCH_HANDSHAKE, fl, n), "the server's flight is the RFC's, byte for byte");
    }
    check(signer.verified, "the RFC's signature verifies over what the server built");
    gtls_conn_debug_secret(c, GTLS_SECRET_CLIENT_HS, secret, sizeof secret, &slen);
    check(same(&v, "server.derive-secret-tls13-c-hs-traffic.expanded", secret, slen), "server: client handshake secret");
    gtls_conn_debug_secret(c, GTLS_SECRET_SERVER_AP, secret, sizeof secret, &slen);
    check(same(&v, "server.derive-secret-tls13-s-ap-traffic.expanded", secret, slen), "server: server application secret");
    {
      const unsigned char * fin = get(&v, "client.send-handshake-record@2.payload", &n);
      size_t at2;
      for (at2 = 0; at2 < n; at2 += chunk == 0 ? n : chunk) {
        size_t k = chunk == 0 ? n : (n - at2 < chunk ? n - at2 : chunk);
        if (gtls_conn_feed(c, GTLS_EPOCH_HANDSHAKE, fin + at2, k, 1700000000) != GTLS_OK) {
          failures++;
        }
      }
    }
    check(gtls_conn_is_connected(c), "the server accepts the RFC's client Finished");
  } else {
    const unsigned char * a;
    const unsigned char * b;
    size_t an = 0, bn = 0, used = 0;
    unsigned char got[64];
    size_t got_n = 0;
    size_t at;

    ch = get(&v, "client.send-handshake-record.complete-record", &n);
    for (at = 0; at < n; at += chunk == 0 ? n : chunk) {
      size_t k = chunk == 0 ? n : (n - at < chunk ? n - at : chunk);
      if (gtls_conn_stream_feed(c, ch + at, k, 1700000000, &used) != GTLS_OK) {
        failures++;
      }
    }
    a = get(&v, "server.send-handshake-record.complete-record", &an);
    b = get(&v, "server.send-handshake-record@2.complete-record", &bn);
    check(stream_is(c, a, an, b, bn), "the server's records: ServerHello plain, the flight protected, byte for byte");
    ch = get(&v, "client.send-handshake-record@2.complete-record", &n);
    for (at = 0; at < n; at += chunk == 0 ? n : chunk) {
      size_t k = chunk == 0 ? n : (n - at < chunk ? n - at : chunk);
      if (gtls_conn_stream_feed(c, ch + at, k, 1700000000, &used) != GTLS_OK) {
        failures++;
      }
    }
    check(gtls_conn_is_connected(c), "the server accepts the RFC's client Finished record");
    ch = get(&v, "client.send-application-data-record.complete-record", &n);
    check(gtls_conn_stream_feed(c, ch, n, 1700000000, &used) == GTLS_OK &&
        gtls_conn_read(c, got, sizeof got, &got_n) == GTLS_OK &&
        same(&v, "client.send-application-data-record.payload", got, got_n), "the server reads the RFC's data record");
    {
      size_t w = 0;
      const unsigned char * pl = get(&v, "server.send-application-data-record.payload", &n);
      gtls_conn_write(c, NULL, 0, &w);
      c->sec.wr.seq = 1;
      check(gtls_conn_write(c, pl, n, &w) == GTLS_OK, "the server writes");
      a = get(&v, "server.send-application-data-record.complete-record", &an);
      check(stream_is(c, a, an, NULL, 0), "the server's data record is the RFC's");
      gtls_conn_close(c);
      a = get(&v, "server.send-alert-record.complete-record", &an);
      check(stream_is(c, a, an, NULL, 0), "the server's close_notify record is the RFC's");
    }
  }
  gtls_conn_free(c);
  gtls_context_free(ctx);
  gcert_key_free(key);
  free(ed_cert);
  free(ed_key);
}

/* --------------------------------------------------------------- the loopback */

static int carry(GTLS_Conn * from, GTLS_Conn * to, size_t chunk, unsigned char * acc, size_t * acc_len) {
  const unsigned char * p = NULL;
  size_t n = 0, at = 0;
  unsigned char * copy;

  if (gtls_conn_stream_output(from, &p, &n) != GTLS_OK || n == 0) {
    return 0;
  }
  copy = malloc(n);
  memcpy(copy, p, n);
  (void)gtls_conn_stream_consume(from, n);
  while (at < n) {
    size_t k = chunk == 0 ? n - at : (n - at < chunk ? n - at : chunk);
    size_t used = 0;
    unsigned char sink[4096];
    size_t got = 0;

    if (gtls_conn_stream_feed(to, copy + at, k, 1700000000, &used) != GTLS_OK) {
      free(copy);
      return -1;
    }
    at += used;
    while (gtls_conn_read(to, sink, sizeof sink, &got) == GTLS_OK && got != 0) {
      if (acc != NULL && *acc_len + got <= 8192u) {
        memcpy(acc + *acc_len, sink, got);
        *acc_len += got;
      }
    }
    if (used == 0) {
      break;
    }
  }
  free(copy);
  return 1;
}

static void loopback(const char * dir, GTLS_Suite suite, GTLS_Group group, size_t chunk, int retry) {
  size_t leaf_len = 0, ca_len = 0, key_len = 0, i;
  unsigned char * leaf = slurp(dir, "ed-leaf.der", &leaf_len);
  unsigned char * ca = slurp(dir, "ed-ca.der", &ca_len);
  unsigned char * kd = slurp(dir, "ed-leaf.pkcs8", &key_len);
  GCERT_TrustStore * trust = NULL;
  GCERT_Key * key = NULL;
  GTLS_ContextConfig cc, sc;
  GTLS_Identity id;
  GTLS_Context * cctx = NULL;
  GTLS_Context * sctx = NULL;
  GTLS_Conn * c = NULL;
  GTLS_Conn * s = NULL;
  const void * chain[1];
  size_t lens[1];
  static const char * const names[1] = {"ed.example.com"};
  GTLS_Group only_p256 = GTLS_GROUP_SECP256R1;
  unsigned char data[3000];
  size_t w = 0, total = 0;
  static unsigned char back[8192];
  size_t back_len = 0;
  int ok;

  if (leaf == NULL || ca == NULL || kd == NULL) {
    failures++;
    return;
  }
  gcert_trust_new(NULL, NULL, &trust);
  gcert_trust_add_der(trust, ca, ca_len);
  gcert_key_from_pkcs8(NULL, kd, key_len, &key);
  gtls_context_config_default(&cc);
  gtls_context_config_default(&sc);
  cc.trust = trust;
  cc.groups = &group;
  cc.group_count = 1;
  if (retry) {
    static const GTLS_Group both[2] = {GTLS_GROUP_X25519, GTLS_GROUP_SECP256R1};
    cc.groups = both;
    cc.group_count = 2;
    sc.groups = &only_p256;
    sc.group_count = 1;
  }
  sc.role = GTLS_ROLE_SERVER;
  sc.suites = &suite;
  sc.suite_count = 1;
  chain[0] = leaf;
  lens[0] = leaf_len;
  memset(&id, 0, sizeof id);
  id.chain = chain;
  id.chain_lens = lens;
  id.chain_count = 1;
  id.key = key;
  id.names = names;
  id.name_count = 1;
  sc.identities = &id;
  sc.identity_count = 1;
  gtls_context_new(NULL, &cc, &cctx);
  gtls_context_new(NULL, &sc, &sctx);
  gtls_conn_new(cctx, "ed.example.com", 14, &c);
  gtls_conn_new_server(sctx, &s);
  gtls_conn_start(c);
  gtls_conn_start(s);
  for (i = 0; i < 100; i++) {
    int a = carry(c, s, chunk, NULL, NULL);
    int b = carry(s, c, chunk, NULL, NULL);

    if (a < 0 || b < 0 || (a == 0 && b == 0)) {
      break;
    }
  }
  check(gtls_conn_is_connected(c) && gtls_conn_is_connected(s), "a real client and server complete a handshake");
  check(gtls_conn_suite(c) == suite && gtls_conn_suite(s) == suite, "and agree on the suite");
  check(gtls_conn_retried(c) == retry && gtls_conn_retried(s) == retry, "and on whether there was a retry");
  for (i = 0; i < sizeof data; i++) {
    data[i] = (unsigned char)(i * 7 + 3);
  }
  ok = gtls_conn_write(c, data, sizeof data, &w) == GTLS_OK && w == sizeof data;
  gtls_conn_key_update(c, 1);
  ok = ok && gtls_conn_write(c, data, 100, &w) == GTLS_OK;
  for (i = 0; i < 50; i++) {
    int a = carry(c, s, chunk, back, &back_len);
    int b = carry(s, c, chunk, NULL, NULL);

    if (a == 0 && b == 0) {
      break;
    }
  }
  total = back_len;
  check(ok && total >= sizeof data && memcmp(back, data, sizeof data) == 0, "application data survives, across a key update");
  gtls_conn_close(c);
  for (i = 0; i < 10; i++) {
    carry(c, s, chunk, NULL, NULL);
    carry(s, c, chunk, NULL, NULL);
  }
  check(gtls_conn_peer_closed(s) && gtls_conn_stream_eof(s) == GTLS_OK, "close_notify ends it cleanly");
  gtls_conn_free(c);
  gtls_conn_free(s);
  gtls_context_free(cctx);
  gtls_context_free(sctx);
  gcert_trust_free(trust);
  gcert_key_free(key);
  free(leaf);
  free(ca);
  free(kd);
}

/* ------------------------------------------------------------- resumption */

static void resumption_known_answers(const char * dir) {
  Vec simple, resumed;
  unsigned char psk[GTLS_HASH_MAX], out[GTLS_HASH_MAX], key[GTLS_HASH_MAX], th[GTLS_HASH_MAX];
  GTLS_Schedule s;
  GTLS_MacScratch mac;
  static const unsigned char nonce[2] = {0, 0};
  size_t n;
  const unsigned char * p;

  if (!load(&simple, dir, "simple-1rtt") || !load(&resumed, dir, "resumed")) {
    failures++;
    return;
  }
  memset(&mac, 0, sizeof mac);
  p = get(&simple, "client.derive-secret-tls13-res-master.expanded", &n);
  check(gtls_resumption_psk(GTLS_HASH_SHA256, p, nonce, sizeof nonce, psk) == GTLS_OK &&
      same(&resumed, "client.extract-secret-early.ikm", psk, 32), "resumption: the PSK a ticket stands for");
  check(gtls_schedule_early_psk(&s, GTLS_HASH_SHA256, psk, 32) == GTLS_OK &&
      same(&resumed, "client.extract-secret-early.secret", s.early, 32), "resumption: the early secret from the PSK");
  check(gtls_schedule_binder_key(&s, key) == GTLS_OK && same(&resumed, "client.calculate-psk-binder.prk", key, 32),
      "resumption: the binder key");
  p = get(&resumed, "client.calculate-psk-binder.binder-hash", &n);
  memcpy(th, p, n);
  check(gtls_binder_compute(GTLS_HASH_SHA256, psk, 32, th, out, &mac) == GTLS_OK &&
      same(&resumed, "client.calculate-psk-binder.finished", out, 32), "resumption: the binder");
  check(gtls_binder_check(GTLS_HASH_SHA256, psk, 32, th, out, 32, &mac) == GTLS_OK, "resumption: the binder checks");
  out[31] ^= 1;
  check(gtls_binder_check(GTLS_HASH_SHA256, psk, 32, th, out, 32, &mac) == GTLS_ERR_MISMATCH, "resumption: a changed binder does not");
  p = get(&resumed, "server.derive-secret-tls13-c-hs-traffic.hash", &n);
  memcpy(th, p, n);
  check(gtls_schedule_handshake(&s, get(&resumed, "server.extract-secret-handshake.ikm", &n), 32, th) == GTLS_OK &&
      same(&resumed, "server.extract-secret-handshake.secret", s.handshake, 32) &&
      same(&resumed, "server.derive-secret-tls13-c-hs-traffic.expanded", s.c_hs, 32) &&
      same(&resumed, "server.derive-secret-tls13-s-hs-traffic.expanded", s.s_hs, 32), "resumption: the handshake secrets");
  p = get(&resumed, "server.derive-secret-tls13-c-ap-traffic.hash", &n);
  memcpy(th, p, n);
  check(gtls_schedule_master(&s, th) == GTLS_OK && same(&resumed, "server.extract-secret-master.secret", s.master, 32) &&
      same(&resumed, "server.derive-secret-tls13-c-ap-traffic.expanded", s.c_ap, 32) &&
      same(&resumed, "server.derive-secret-tls13-s-ap-traffic.expanded", s.s_ap, 32), "resumption: the application secrets");
  {
    /* The RFC's NewSessionTicket and its ServerHello, rebuilt. */
    const unsigned char * nst = get(&simple, "server.construct-a-newsessionticket-handshake-message.newsessionticket", &n);
    GTLS_NewSessionTicket t;
    GTLS_Limits lim;
    GTLS_Buf b;
    GTLS_ServerHelloParams sp;
    const unsigned char * sh;
    size_t shn, kxn;
    const unsigned char * kx;

    gtls_limits_default(&lim);
    check(gtls_msg_parse_new_session_ticket(nst + 4, n - 4, &lim, &t) == GTLS_OK, "resumption: the RFC's NewSessionTicket parses");
    gtls_buf_init(&b, NULL, 4096);
    check(gtls_msg_build_new_session_ticket(&t, &b) == GTLS_OK && b.len == n && memcmp(b.data, nst, n) == 0,
        "resumption: and builds back byte for byte");
    gtls_buf_free(&b);
    sh = get(&resumed, "server.construct-a-serverhello-handshake-message.serverhello", &shn);
    kx = get(&resumed, "server.create-an-ephemeral-x25519-key-pair.public-key", &kxn);
    memset(&sp, 0, sizeof sp);
    memcpy(sp.random, sh + 6, 32);
    sp.suite = GTLS_SUITE_AES_128_GCM_SHA256;
    sp.group = GTLS_GROUP_X25519;
    sp.key_exchange = kx;
    sp.key_exchange_len = kxn;
    sp.psk = 1;
    gtls_buf_init(&b, NULL, 4096);
    check(gtls_msg_build_server_hello(&sp, &b) == GTLS_OK && b.len == shn && memcmp(b.data, sh, shn) == 0,
        "resumption: the RFC's ServerHello with its pre_shared_key, byte for byte");
    gtls_buf_free(&b);
  }
  gtls_schedule_wipe(&s);
}

static void early_known_answers(const char * dir) {
  Vec v;
  GTLS_Schedule s;
  GTLS_RecordKeys k;
  unsigned char th[32], key[GTLS_KEY_MAX], iv[GTLS_IV_LEN];
  const unsigned char * hello;
  const unsigned char * ikm;
  size_t hn, n;

  if (!load(&v, dir, "resumed")) {
    failures++;
    return;
  }
  hello = get(&v, "client.send-handshake-record.payload", &hn);
  check(gsec_sha256(hello, hn, th) == GSEC_OK && same(&v, "client.derive-secret-tls13-c-e-traffic.hash", th, 32),
      "early data: the transcript hash is the whole ClientHello");
  ikm = get(&v, "client.extract-secret-early.ikm", &n);
  check(gtls_schedule_early_psk(&s, GTLS_HASH_SHA256, ikm, 32) == GTLS_OK &&
      gtls_schedule_early_traffic(&s, th) == GTLS_OK &&
      same(&v, "client.derive-secret-tls13-c-e-traffic.expanded", s.c_e, 32), "early data: client_early_traffic_secret");
  check(gtls_traffic_keys(GTLS_SUITE_AES_128_GCM_SHA256, s.c_e, key, iv) == GTLS_OK &&
      same(&v, "client.derive-write-traffic-keys-for-early-application-data.key-expanded", key, 16) &&
      same(&v, "client.derive-write-traffic-keys-for-early-application-data.iv-expanded", iv, GTLS_IV_LEN),
      "early data: the key and IV of the early records");
  check(gtls_record_keys_from_secret(&k, GTLS_SUITE_AES_128_GCM_SHA256, s.c_e, 0) == GTLS_OK &&
      seal_expect(&k, &v, "client.send-application-data-record.complete-record",
          "client.send-application-data-record.payload", GTLS_CT_APPLICATION_DATA) &&
      seal_expect(&k, &v, "client.send-handshake-record@2.complete-record", "client.send-handshake-record@2.payload",
          GTLS_CT_HANDSHAKE), "early data: the early record and EndOfEarlyData, sealed, the second under one key");
  check(gtls_record_keys_from_secret(&k, GTLS_SUITE_AES_128_GCM_SHA256, s.c_e, 0) == GTLS_OK &&
      open_expect(&k, &v, "client.send-application-data-record.complete-record",
          "client.send-application-data-record.payload", GTLS_CT_APPLICATION_DATA) &&
      open_expect(&k, &v, "client.send-handshake-record@2.complete-record", "client.send-handshake-record@2.payload",
          GTLS_CT_HANDSHAKE), "early data: and opened in order");
  {
    GTLS_Buf b;
    const unsigned char * eoed = get(&v, "client.construct-an-endofearlydata-handshake-message.endofearlydata", &n);

    gtls_buf_init(&b, NULL, 64);
    check(gtls_msg_build_end_of_early_data(&b) == GTLS_OK && b.len == n && memcmp(b.data, eoed, n) == 0,
        "early data: EndOfEarlyData is the RFC's four bytes");
    gtls_buf_free(&b);
  }
  gtls_schedule_wipe(&s);
}

static int probe_replay(void * user, const unsigned char id[32], int64_t issued, uint32_t age_ms) {
  (void)user;
  (void)id;
  (void)issued;
  (void)age_ms;
  return 1;
}

static void resume_loopback(const char * dir, GTLS_Suite suite, GTLS_Group group, size_t chunk, int retry,
    int early) {
  size_t leaf_len = 0, ca_len = 0, key_len = 0, i, round;
  unsigned char * leaf = slurp(dir, "ed-leaf.der", &leaf_len);
  unsigned char * ca = slurp(dir, "ed-ca.der", &ca_len);
  unsigned char * kd = slurp(dir, "ed-leaf.pkcs8", &key_len);
  GCERT_TrustStore * trust = NULL;
  GCERT_Key * key = NULL;
  GTLS_ContextConfig cc, sc;
  GTLS_Identity id;
  GTLS_Context * cctx = NULL;
  GTLS_Context * sctx = NULL;
  GTLS_Conn * c = NULL;
  GTLS_Conn * s = NULL;
  GTLS_Session * session = NULL;
  GTLS_TicketKey tk;
  const void * chain[1];
  size_t lens[1];
  static const char * const names[1] = {"ed.example.com"};
  GTLS_Group only_p256 = GTLS_GROUP_SECP256R1;
  unsigned char data[2000];
  static unsigned char back[8192];
  size_t back_len = 0, w = 0;
  int resumed_ok = 0;

  if (leaf == NULL || ca == NULL || kd == NULL) {
    failures++;
    return;
  }
  gcert_trust_new(NULL, NULL, &trust);
  gcert_trust_add_der(trust, ca, ca_len);
  gcert_key_from_pkcs8(NULL, kd, key_len, &key);
  gtls_context_config_default(&cc);
  gtls_context_config_default(&sc);
  cc.trust = trust;
  cc.groups = &group;
  cc.group_count = 1;
  if (retry) {
    static const GTLS_Group both[2] = {GTLS_GROUP_X25519, GTLS_GROUP_SECP256R1};
    cc.groups = both;
    cc.group_count = 2;
    sc.groups = &only_p256;
    sc.group_count = 1;
  }
  sc.role = GTLS_ROLE_SERVER;
  sc.suites = &suite;
  sc.suite_count = 1;
  chain[0] = leaf;
  lens[0] = leaf_len;
  memset(&id, 0, sizeof id);
  id.chain = chain;
  id.chain_lens = lens;
  id.chain_count = 1;
  id.key = key;
  id.names = names;
  id.name_count = 1;
  sc.identities = &id;
  sc.identity_count = 1;
  memset(&tk, 0x5a, sizeof tk);
  tk.not_before = 1699990000;
  tk.not_after = 1700100000;
  sc.ticket_keys = &tk;
  sc.ticket_key_count = 1;
  if (early) {
    cc.max_early_data = 1000;
    sc.max_early_data = 1000;
    sc.anti_replay = probe_replay;
  }
  gtls_context_new(NULL, &cc, &cctx);
  gtls_context_new(NULL, &sc, &sctx);
  for (round = 0; round < 2; round++) {
    back_len = 0;
    gtls_conn_new(cctx, "ed.example.com", 14, &c);
    gtls_conn_new_server(sctx, &s);
    if (round == 1) {
      check(gtls_conn_set_session(c, session, 1700000000) == GTLS_OK, "a session is offered");
    }
    gtls_conn_start(c);
    gtls_conn_start(s);
    if (early && round == 1) {
      unsigned char eb[300];

      for (i = 0; i < sizeof eb; i++) {
        eb[i] = (unsigned char)(i * 13 + 5);
      }
      check(gtls_conn_write_early(c, eb, sizeof eb, &w) == GTLS_OK && w == sizeof eb, "early data is written");
    }
    for (i = 0; i < 100; i++) {
      int a = carry(c, s, chunk, NULL, NULL);
      int b = carry(s, c, chunk, NULL, NULL);

      if (a < 0 || b < 0 || (a == 0 && b == 0)) {
        break;
      }
    }
    check(gtls_conn_is_connected(c) && gtls_conn_is_connected(s), round == 0 ? "a full handshake with a ticket" : "a resumed handshake");
    if (early && round == 1) {
      unsigned char eb[300], got[400];
      size_t gn = 0, rn = 0;
      GTLS_EarlyStatus cs = GTLS_EARLY_NONE, ss = GTLS_EARLY_NONE;
      GTLS_Result cr = gtls_conn_early_status(c, &cs);

      for (i = 0; i < sizeof eb; i++) {
        eb[i] = (unsigned char)(i * 13 + 5);
      }
      (void)gtls_conn_early_status(s, &ss);
      if (retry) {
        check(cr == GTLS_ERR_EARLY_REJECTED && cs == GTLS_EARLY_REJECTED && ss == GTLS_EARLY_REJECTED,
            "early data after a retry: refused, and the client is told");
        check(gtls_conn_early_resend(c, got, sizeof got, &rn) == GTLS_OK && rn == sizeof eb && memcmp(got, eb, rn) == 0,
            "early data after a retry: handed back");
        check(gtls_conn_read_early(s, got, sizeof got, &gn) == GTLS_OK && gn == 0, "early data after a retry: none delivered");
      } else {
        check(cr == GTLS_OK && cs == GTLS_EARLY_ACCEPTED && ss == GTLS_EARLY_ACCEPTED, "early data: accepted by both");
        check(gtls_conn_read_early(s, got, sizeof got, &gn) == GTLS_OK && gn == sizeof eb && memcmp(got, eb, gn) == 0,
            "early data: delivered through the early read");
        check(gtls_conn_read_early(s, got, sizeof got, &gn) == GTLS_OK && gn == 0, "early data: once");
      }
    }
    check(gtls_conn_resumed(c) == (int)round && gtls_conn_resumed(s) == (int)round, round == 0 ? "is not resumed" : "is resumed on both ends");
    check(gtls_conn_retried(c) == retry, "with or without a retry");
    check(gtls_conn_session_count(c) == 1, "and a ticket arrives");
    if (round == 1) {
      const unsigned char * der = NULL;
      size_t dn = 1;

      check(gtls_conn_peer_certificate(c, &der, &dn) == GTLS_OK && dn == 0, "no certificate in the abbreviated one");
    }
    if (round == 0) {
      check(gtls_conn_session(c, 0, &session) == GTLS_OK, "the client keeps the session");
    }
    for (i = 0; i < sizeof data; i++) {
      data[i] = (unsigned char)(i * 11 + round);
    }
    check(gtls_conn_write(c, data, sizeof data, &w) == GTLS_OK && w == sizeof data, "the client writes");
    for (i = 0; i < 30; i++) {
      int a = carry(c, s, chunk, back, &back_len);
      int b = carry(s, c, chunk, NULL, NULL);

      if (a == 0 && b == 0) {
        break;
      }
    }
    check(back_len == sizeof data && memcmp(back, data, sizeof data) == 0, "application data arrives");
    resumed_ok += gtls_conn_resumed(c);
    gtls_conn_free(c);
    gtls_conn_free(s);
    c = NULL;
    s = NULL;
  }
  check(resumed_ok == 1, "only the second connection was resumed");
  gtls_session_free(session);
  gtls_context_free(cctx);
  gtls_context_free(sctx);
  gcert_trust_free(trust);
  gcert_key_free(key);
  free(leaf);
  free(ca);
  free(kd);
}

int main(int argc, char ** argv) {
  const char * dir = argc > 1 ? argv[1] : ".";
  size_t chunks[] = {0, 1, 7};
  size_t i;

  check(gtls_selftest() == GTLS_OK, "the library's self test");
  printf("sizeof(size_t) %zu\n", sizeof(size_t));
  for (i = 0; i < 3; i++) {
    printf("-- simple 1-RTT, feed size %zu\n", chunks[i]);
    trace(dir, "simple-1rtt", 0, chunks[i]);
    printf("-- HelloRetryRequest, feed size %zu\n", chunks[i]);
    trace(dir, "hrr", 1, chunks[i]);
  }
  printf("-- records\n");
  records(dir);
  for (i = 0; i < 3; i++) {
    printf("-- the RFC's server as messages, feed size %zu\n", chunks[i]);
    rfc_server(dir, 0, chunks[i]);
    printf("-- the RFC's server as records, feed size %zu\n", chunks[i]);
    rfc_server(dir, 1, chunks[i]);
  }
  {
    static const GTLS_Suite suites[3] = {GTLS_SUITE_AES_128_GCM_SHA256, GTLS_SUITE_AES_256_GCM_SHA384,
        GTLS_SUITE_CHACHA20_POLY1305_SHA256};
    static const GTLS_Group groups[3] = {GTLS_GROUP_X25519, GTLS_GROUP_SECP256R1, GTLS_GROUP_SECP384R1};
    size_t j;

    for (i = 0; i < 3; i++) {
      for (j = 0; j < 3; j++) {
        printf("-- loopback, suite %zu, group %zu, feed size 5\n", i, j);
        loopback(dir, suites[i], groups[j], 5, 0);
      }
    }
    printf("-- loopback with a HelloRetryRequest, whole and one byte at a time\n");
    loopback(dir, GTLS_SUITE_AES_128_GCM_SHA256, GTLS_GROUP_X25519, 0, 1);
    loopback(dir, GTLS_SUITE_CHACHA20_POLY1305_SHA256, GTLS_GROUP_X25519, 1, 1);
    printf("-- RFC 8448's resumed handshake\n");
    resumption_known_answers(dir);
    for (i = 0; i < 3; i++) {
      for (j = 0; j < 3; j++) {
        printf("-- resumption, suite %zu, group %zu, feed size 5\n", i, j);
        resume_loopback(dir, suites[i], groups[j], 5, 0, 0);
      }
    }
    printf("-- resumption after a HelloRetryRequest, whole and one byte at a time\n");
    resume_loopback(dir, GTLS_SUITE_AES_256_GCM_SHA384, GTLS_GROUP_X25519, 0, 1, 0);
    resume_loopback(dir, GTLS_SUITE_CHACHA20_POLY1305_SHA256, GTLS_GROUP_X25519, 1, 1, 0);
    printf("-- RFC 8448's 0-RTT trace\n");
    early_known_answers(dir);
    for (i = 0; i < 3; i++) {
      for (j = 0; j < 3; j++) {
        printf("-- early data, suite %zu, group %zu, feed size 5\n", i, j);
        resume_loopback(dir, suites[i], groups[j], 5, 0, 1);
      }
    }
    printf("-- early data, whole and one byte at a time\n");
    resume_loopback(dir, GTLS_SUITE_AES_128_GCM_SHA256, GTLS_GROUP_X25519, 0, 0, 1);
    resume_loopback(dir, GTLS_SUITE_CHACHA20_POLY1305_SHA256, GTLS_GROUP_SECP256R1, 1, 0, 1);
    printf("-- early data refused after a HelloRetryRequest, whole and one byte at a time\n");
    resume_loopback(dir, GTLS_SUITE_AES_256_GCM_SHA384, GTLS_GROUP_X25519, 0, 1, 1);
    resume_loopback(dir, GTLS_SUITE_CHACHA20_POLY1305_SHA256, GTLS_GROUP_X25519, 1, 1, 1);
  }
  printf("%d checks failed\n", failures);
  return failures == 0 ? 0 : 1;
}
