/*
 *  OpenVPN -- An application to securely tunnel IP networks
 *             over a single TCP/UDP port, with support for SSL/TLS-based
 *             session authentication and key exchange,
 *             packet encryption, packet authentication, and
 *             packet compression.
 *
 *  Copyright (C) 2002-2010 OpenVPN Technologies, Inc. <sales@openvpn.net>
 *  Copyright (C) 2010 Fox Crypto B.V. <openvpn@fox-it.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2
 *  as published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program (see the file COPYING included with this
 *  distribution); if not, write to the Free Software Foundation, Inc.,
 *  59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/**
 * @file Control Channel Verification Module PolarSSL backend
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#elif defined(_MSC_VER)
#include "config-msvc.h"
#endif

#include "syshead.h"

#if defined(ENABLE_CRYPTO) && defined(ENABLE_CRYPTO_POLARSSL)

#include "crypto_polarssl.h"
#include "ssl_verify.h"
#include <polarssl/asn1.h>
#include <polarssl/error.h>
#include <polarssl/bignum.h>
#include <polarssl/oid.h>
#include <polarssl/sha1.h>

#define MAX_SUBJECT_LENGTH 256

int
verify_callback (void *session_obj, x509_crt *cert, int cert_depth,
    int *flags)
{
  struct tls_session *session = (struct tls_session *) session_obj;
  struct gc_arena gc = gc_new();

  ASSERT (cert);
  ASSERT (session);

  session->verified = false;

  /* Remember certificate hash */
  cert_hash_remember (session, cert_depth, x509_get_sha1_hash(cert, &gc));

  /* did peer present cert which was signed by our root cert? */
  if (*flags != 0)
    {
      char *subject = x509_get_subject(cert, &gc);

      if (subject)
	msg (D_TLS_ERRORS, "VERIFY ERROR: depth=%d, flags=%x, %s", cert_depth, *flags, subject);
      else
	msg (D_TLS_ERRORS, "VERIFY ERROR: depth=%d, flags=%x, could not extract X509 "
	      "subject string from certificate", *flags, cert_depth);

      /* Leave flags set to non-zero to indicate that the cert is not ok */
    }
  else if (SUCCESS != verify_cert(session, cert, cert_depth))
    {
      *flags |= BADCERT_OTHER;
    }

  gc_free(&gc);

  /*
   * PolarSSL-1.2.0+ expects 0 on anything except fatal errors.
   */
  return 0;
}

#ifdef ENABLE_X509ALTUSERNAME
# warning "X509 alt user name not yet supported for PolarSSL"
#endif

result_t
backend_x509_get_username (char *cn, int cn_len,
    char *x509_username_field, x509_crt *cert)
{
  x509_name *name;

  ASSERT( cn != NULL );

  name = &cert->subject;

  /* Find common name */
  while( name != NULL )
  {
      if( memcmp( name->oid.p, OID_AT_CN, OID_SIZE(OID_AT_CN) ) == 0)
	break;

      name = name->next;
  }

  /* Not found, return an error if this is the peer's certificate */
  if( name == NULL )
      return FAILURE;

  /* Found, extract CN */
  if (cn_len > name->val.len)
    {
      memcpy( cn, name->val.p, name->val.len );
      cn[name->val.len] = '\0';
    }
  else
    {
      memcpy( cn, name->val.p, cn_len);
      cn[cn_len-1] = '\0';
    }

  return SUCCESS;
}

char *
backend_x509_get_serial (openvpn_x509_cert_t *cert, struct gc_arena *gc)
{
  char *buf = NULL;
  size_t buflen = 0;
  mpi serial_mpi = { 0 };

  /* Transform asn1 integer serial into PolarSSL MPI */
  mpi_init(&serial_mpi);
  if (!polar_ok(mpi_read_binary(&serial_mpi, cert->serial.p, cert->serial.len)))
    {
      msg(M_WARN, "Failed to retrieve serial from certificate.");
      return NULL;
    }

  /* Determine decimal representation length, allocate buffer */
  mpi_write_string(&serial_mpi, 10, buf, &buflen);
  buf = gc_malloc(buflen, true, gc);

  /* Write MPI serial as decimal string into buffer */
  if (!polar_ok(mpi_write_string(&serial_mpi, 10, buf, &buflen)))
    {
      msg(M_WARN, "Failed to write serial to string.");
      return NULL;
    }

  return buf;
}

char *
backend_x509_get_serial_hex (openvpn_x509_cert_t *cert, struct gc_arena *gc)
{
  char *buf = NULL;
  size_t len = cert->serial.len * 3 + 1;

  buf = gc_malloc(len, true, gc);

  if(x509_serial_gets(buf, len-1, &cert->serial) < 0)
    buf = NULL;

  return buf;
}

unsigned char *
x509_get_sha1_hash (x509_crt *cert, struct gc_arena *gc)
{
  unsigned char *sha1_hash = gc_malloc(SHA_DIGEST_LENGTH, false, gc);
  sha1(cert->raw.p, cert->raw.len, sha1_hash);
  return sha1_hash;
}

char *
x509_get_subject(x509_crt *cert, struct gc_arena *gc)
{
  char tmp_subject[MAX_SUBJECT_LENGTH] = {0};
  char *subject = NULL;

  int ret = 0;

  ret = x509_dn_gets( tmp_subject, MAX_SUBJECT_LENGTH-1, &cert->subject );
  if (ret > 0)
    {
      /* Allocate the required space for the subject */
      subject = string_alloc(tmp_subject, gc);
    }

  return subject;
}

static void
do_setenv_x509 (struct env_set *es, const char *name, char *value, int depth)
{
  char *name_expand;
  size_t name_expand_size;

  string_mod (value, CC_ANY, CC_CRLF, '?');
  msg (D_X509_ATTR, "X509 ATTRIBUTE name='%s' value='%s' depth=%d", name, value, depth);
  name_expand_size = 64 + strlen (name);
  name_expand = (char *) malloc (name_expand_size);
  check_malloc_return (name_expand);
  openvpn_snprintf (name_expand, name_expand_size, "X509_%d_%s", depth, name);
  setenv_str (es, name_expand, value);
  free (name_expand);
}

static char *
asn1_buf_to_c_string(const asn1_buf *orig, struct gc_arena *gc)
{
  size_t i;
  char *val;

  for (i = 0; i < orig->len; ++i)
    if (orig->p[i] == '\0')
      return "ERROR: embedded null value";
  val = gc_malloc(orig->len+1, false, gc);
  memcpy(val, orig->p, orig->len);
  val[orig->len] = '\0';
  return val;
}

static void
do_setenv_name(struct env_set *es, const struct x509_track *xt,
	       const x509_crt *cert, int depth, struct gc_arena *gc)
{
  const x509_name *xn;
  for (xn = &cert->subject; xn != NULL; xn = xn->next)
    {
      const char *xn_short_name = NULL;
      if (0 == oid_get_attr_short_name (&xn->oid, &xn_short_name) &&
	  0 == strcmp (xt->name, xn_short_name))
	{
	  char *val_str = asn1_buf_to_c_string (&xn->val, gc);
	  do_setenv_x509 (es, xt->name, val_str, depth);
	}
    }
}

void
x509_track_add (const struct x509_track **ll_head, const char *name, int msglevel, struct gc_arena *gc)
{
  struct x509_track *xt;
  ALLOC_OBJ_CLEAR_GC (xt, struct x509_track, gc);
  if (*name == '+')
    {
      xt->flags |= XT_FULL_CHAIN;
      ++name;
    }
  xt->name = name;
  xt->next = *ll_head;
  *ll_head = xt;
}

void
x509_setenv_track (const struct x509_track *xt, struct env_set *es, const int depth, x509_crt *cert)
{
  struct gc_arena gc = gc_new();
  while (xt)
    {
      if (depth == 0 || (xt->flags & XT_FULL_CHAIN))
	{
	  if (0 == strcmp(xt->name, "SHA1"))
	    {
	      /* SHA1 fingerprint is not part of X509 structure */
	      unsigned char *sha1_hash = x509_get_sha1_hash(cert, &gc);
	      char *sha1_fingerprint = format_hex_ex(sha1_hash, SHA_DIGEST_LENGTH, 0, 1 | FHE_CAPS, ":", &gc);
	      do_setenv_x509(es, xt->name, sha1_fingerprint, depth);
	    }
	  else
	    {
	      do_setenv_name(es, xt, cert, depth, &gc);
	    }
	}
      xt = xt->next;
    }
  gc_free(&gc);
}

/*
 * Save X509 fields to environment, using the naming convention:
 *
 * X509_{cert_depth}_{name}={value}
 */
void
x509_setenv (struct env_set *es, int cert_depth, openvpn_x509_cert_t *cert)
{
  int i;
  unsigned char c;
  const x509_name *name;
  char s[128];

  name = &cert->subject;

  memset( s, 0, sizeof( s ) );

  while( name != NULL )
    {
      char name_expand[64+8];
      const char *shortname;

      if( 0 == oid_get_attr_short_name(&name->oid, &shortname) )
	{
	  openvpn_snprintf (name_expand, sizeof(name_expand), "X509_%d_%s",
	      cert_depth, shortname);
	}
      else
	{
	  openvpn_snprintf (name_expand, sizeof(name_expand), "X509_%d_\?\?",
	      cert_depth);
	}

      for( i = 0; i < name->val.len; i++ )
	{
	  if( i >= (int) sizeof( s ) - 1 )
	      break;

	  c = name->val.p[i];
	  if( c < 32 || c == 127 || ( c > 128 && c < 160 ) )
	       s[i] = '?';
	  else s[i] = c;
	}
	s[i] = '\0';

	/* Check both strings, set environment variable */
	string_mod (name_expand, CC_PRINT, CC_CRLF, '_');
	string_mod ((char*)s, CC_PRINT, CC_CRLF, '_');
	setenv_str_incr (es, name_expand, (char*)s);

	name = name->next;
    }
}

result_t
x509_verify_ns_cert_type(const x509_crt *cert, const int usage)
{
  if (usage == NS_CERT_CHECK_NONE)
    return SUCCESS;
  if (usage == NS_CERT_CHECK_CLIENT)
    return ((cert->ext_types & EXT_NS_CERT_TYPE)
	&& (cert->ns_cert_type & NS_CERT_TYPE_SSL_CLIENT)) ? SUCCESS : FAILURE;
  if (usage == NS_CERT_CHECK_SERVER)
    return ((cert->ext_types & EXT_NS_CERT_TYPE)
	&& (cert->ns_cert_type & NS_CERT_TYPE_SSL_SERVER)) ? SUCCESS : FAILURE;

  return FAILURE;
}

result_t
x509_verify_cert_ku (x509_crt *cert, const unsigned * const expected_ku,
    int expected_len)
{
  result_t fFound = FAILURE;

  if(!(cert->ext_types & EXT_KEY_USAGE))
    {
      msg (D_HANDSHAKE, "Certificate does not have key usage extension");
    }
  else
    {
      int i;
      unsigned nku = cert->key_usage;

      msg (D_HANDSHAKE, "Validating certificate key usage");
      for (i=0; SUCCESS != fFound && i<expected_len; i++)
	{
	  if (expected_ku[i] != 0)
	    {
	      msg (D_HANDSHAKE, "++ Certificate has key usage  %04x, expects "
		  "%04x", nku, expected_ku[i]);

	      if (nku == expected_ku[i])
		{
		  fFound = SUCCESS;
		}
	    }
	}
    }
  return fFound;
}

result_t
x509_verify_cert_eku (x509_crt *cert, const char * const expected_oid)
{
  result_t fFound = FAILURE;

  if (!(cert->ext_types & EXT_EXTENDED_KEY_USAGE))
    {
      msg (D_HANDSHAKE, "Certificate does not have extended key usage extension");
    }
  else
    {
      x509_sequence *oid_seq = &(cert->ext_key_usage);

      msg (D_HANDSHAKE, "Validating certificate extended key usage");
      while (oid_seq != NULL)
	{
	  x509_buf *oid = &oid_seq->buf;
	  char oid_num_str[1024];
	  const char *oid_str;

	  if (0 == oid_get_extended_key_usage( oid, &oid_str ))
	    {
	      msg (D_HANDSHAKE, "++ Certificate has EKU (str) %s, expects %s",
		  oid_str, expected_oid);
	      if (!strcmp (expected_oid, oid_str))
		{
		  fFound = SUCCESS;
		  break;
		}
	    }

	  if (0 < oid_get_numeric_string( oid_num_str,
	      sizeof (oid_num_str), oid))
	    {
	      msg (D_HANDSHAKE, "++ Certificate has EKU (oid) %s, expects %s",
		  oid_num_str, expected_oid);
	      if (!strcmp (expected_oid, oid_num_str))
		{
		  fFound = SUCCESS;
		  break;
		}
	    }
	  oid_seq = oid_seq->next;
	}
    }

    return fFound;
}

result_t
x509_write_pem(FILE *peercert_file, x509_crt *peercert)
{
    msg (M_WARN, "PolarSSL does not support writing peer certificate in PEM format");
    return FAILURE;
}

/*
 * check peer cert against CRL
 */
result_t
x509_verify_crl(const char *crl_file, const char* crl_inline,
                x509_crt *cert, const char *subject)
{
  result_t retval = FAILURE;
  x509_crl crl = {0};
  struct gc_arena gc = gc_new();
  char *serial;

  if (!strcmp (crl_file, INLINE_FILE_TAG) && crl_inline)
    {
      if (!polar_ok(x509_crl_parse(&crl, crl_inline, strlen(crl_inline))))
        {
           msg (M_WARN, "CRL: cannot parse inline CRL");
           goto end;
        }
    }
  else
    {
      if (!polar_ok(x509_crl_parse_file(&crl, crl_file)))
      {
          msg (M_WARN, "CRL: cannot read CRL from file %s", crl_file);
          goto end;
      }
  }

  if(cert->issuer_raw.len != crl.issuer_raw.len ||
      memcmp(crl.issuer_raw.p, cert->issuer_raw.p, crl.issuer_raw.len) != 0)
    {
      msg (M_WARN, "CRL: CRL %s is from a different issuer than the issuer of "
	  "certificate %s", crl_file, subject);
      retval = SUCCESS;
      goto end;
    }

  if (!polar_ok(x509_crt_revoked(cert, &crl)))
    {
      serial = backend_x509_get_serial_hex(cert, &gc);
      msg (D_HANDSHAKE, "CRL CHECK FAILED: %s (serial %s) is REVOKED", subject, (serial ? serial : "NOT AVAILABLE"));
      goto end;
    }

  retval = SUCCESS;
  msg (D_HANDSHAKE, "CRL CHECK OK: %s",subject);

end:
  gc_free(&gc);
  x509_crl_free(&crl);
  return retval;
}

#endif /* #if defined(ENABLE_CRYPTO) && defined(ENABLE_CRYPTO_POLARSSL) */
