/* $OpenLDAP$ */
/* This work is part of OpenLDAP Software <http://www.openldap.org/>.
 *
 * Copyright 1998-2024 The OpenLDAP Foundation.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted only as authorized by the OpenLDAP
 * Public License.
 *
 * A copy of this license is available in the file LICENSE in the
 * top-level directory of the distribution or, alternatively, at
 * <http://www.OpenLDAP.org/license.html>.
 */

/*
 * int lutil_passwd(
 *	const struct berval *passwd,
 *	const struct berval *cred,
 *	const char **schemes )
 *
 * Returns true if user supplied credentials (cred) matches
 * the stored password (passwd). 
 *
 * Due to the use of the crypt(3) function 
 * this routine is NOT thread-safe.
 */

#include "portable.h"

#include <stdio.h>
#include <ac/stdlib.h>
#include <ac/string.h>
#include <ac/unistd.h>
#include <ac/param.h>

#ifdef SLAPD_CRYPT
# include <ac/crypt.h>

# if defined( HAVE_GETPWNAM ) && defined( HAVE_STRUCT_PASSWD_PW_PASSWD )
#  ifdef HAVE_SHADOW_H
#	include <shadow.h>
#  endif
#  ifdef HAVE_PWD_H
#	include <pwd.h>
#  endif
#  ifdef HAVE_AIX_SECURITY
#	include <userpw.h>
#  endif
# endif
#endif

#include <lber.h>

#include "ldap_pvt.h"
#include "lber_pvt.h"

#include "lutil_md5.h"
#include "lutil_sha1.h"
#include "lutil.h"

static const unsigned char crypt64[] =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz1234567890./";

#ifdef SLAPD_CRYPT
static char *salt_format = NULL;
static lutil_cryptfunc lutil_crypt;
lutil_cryptfunc *lutil_cryptptr = lutil_crypt;
#endif

/* KLUDGE:
 *  chk_fn is NULL iff name is {CLEARTEXT}
 *	otherwise, things will break
 */
struct pw_scheme {
	struct berval name;
	LUTIL_PASSWD_CHK_FUNC *chk_fn;
	LUTIL_PASSWD_HASH_FUNC *hash_fn;
};

struct pw_slist {
	struct pw_slist *next;
	struct pw_scheme s;
};

/* password check routines */

#define	SALT_SIZE	4

static LUTIL_PASSWD_CHK_FUNC chk_md5;
static LUTIL_PASSWD_CHK_FUNC chk_smd5;
static LUTIL_PASSWD_HASH_FUNC hash_smd5;
static LUTIL_PASSWD_HASH_FUNC hash_md5;


#ifdef LUTIL_SHA1_BYTES
static LUTIL_PASSWD_CHK_FUNC chk_ssha1;
static LUTIL_PASSWD_CHK_FUNC chk_sha1;
static LUTIL_PASSWD_HASH_FUNC hash_sha1;
static LUTIL_PASSWD_HASH_FUNC hash_ssha1;
#endif


#ifdef SLAPD_CRYPT
static LUTIL_PASSWD_CHK_FUNC chk_crypt;
static LUTIL_PASSWD_HASH_FUNC hash_crypt;

#if defined( HAVE_GETPWNAM ) && defined( HAVE_STRUCT_PASSWD_PW_PASSWD )
static LUTIL_PASSWD_CHK_FUNC chk_unix;
#endif
#endif

/* password hash routines */

#ifdef SLAPD_CLEARTEXT
static LUTIL_PASSWD_HASH_FUNC hash_clear;
#endif

static struct pw_slist *pw_schemes;
static int pw_inited;

static const struct pw_scheme pw_schemes_default[] =
{
#ifdef LUTIL_SHA1_BYTES
	{ BER_BVC("{SSHA}"),		chk_ssha1, hash_ssha1 },
	{ BER_BVC("{SHA}"),			chk_sha1, hash_sha1 },
#endif

	{ BER_BVC("{SMD5}"),		chk_smd5, hash_smd5 },
	{ BER_BVC("{MD5}"),			chk_md5, hash_md5 },

#ifdef SLAPD_CRYPT
	{ BER_BVC("{CRYPT}"),		chk_crypt, hash_crypt },
# if defined( HAVE_GETPWNAM ) && defined( HAVE_STRUCT_PASSWD_PW_PASSWD )
	{ BER_BVC("{UNIX}"),		chk_unix, NULL },
# endif
#endif

#ifdef SLAPD_CLEARTEXT
	/* pseudo scheme */
	{ BER_BVC("{CLEARTEXT}"),	NULL, hash_clear },
#endif

	{ BER_BVNULL, NULL, NULL }
};

int lutil_passwd_add(
	struct berval *scheme,
	LUTIL_PASSWD_CHK_FUNC *chk,
	LUTIL_PASSWD_HASH_FUNC *hash )
{
	struct pw_slist *ptr;

	if (!pw_inited) lutil_passwd_init();

	ptr = ber_memalloc( sizeof( struct pw_slist ));
	if (!ptr) return -1;
	ptr->next = pw_schemes;
	ptr->s.name = *scheme;
	ptr->s.chk_fn = chk;
	ptr->s.hash_fn = hash;
	pw_schemes = ptr;
	return 0;
}

void lutil_passwd_init()
{
	struct pw_scheme *s;

	pw_inited = 1;

	for( s=(struct pw_scheme *)pw_schemes_default; s->name.bv_val; s++) {
		if ( lutil_passwd_add( &s->name, s->chk_fn, s->hash_fn ) ) break;
	}
}

void lutil_passwd_destroy()
{
	struct pw_slist *ptr, *next;

	for( ptr=pw_schemes; ptr; ptr=next ) {
		next = ptr->next;
		ber_memfree( ptr );
	}
}

static const struct pw_scheme *get_scheme(
	const char* scheme )
{
	struct pw_slist *pws;
	struct berval bv;

	if (!pw_inited) lutil_passwd_init();

	bv.bv_val = strchr( scheme, '}' );
	if ( !bv.bv_val )
		return NULL;

	bv.bv_len = bv.bv_val - scheme + 1;
	bv.bv_val = (char *) scheme;

	for( pws=pw_schemes; pws; pws=pws->next ) {
		if ( ber_bvstrcasecmp(&bv, &pws->s.name ) == 0 ) {
			return &(pws->s);
		}
	}

	return NULL;
}

int lutil_passwd_scheme(
	const char* scheme )
{
	if( scheme == NULL ) {
		return 0;
	}

	return get_scheme(scheme) != NULL;
}


static int is_allowed_scheme( 
	const char* scheme,
	const char** schemes )
{
	int i;

	if( schemes == NULL ) return 1;

	for( i=0; schemes[i] != NULL; i++ ) {
		if( strcasecmp( scheme, schemes[i] ) == 0 ) {
			return 1;
		}
	}
	return 0;
}

static struct berval *passwd_scheme(
	const struct pw_scheme *scheme,
	const struct berval * passwd,
	struct berval *bv,
	const char** allowed )
{
	if( !is_allowed_scheme( scheme->name.bv_val, allowed ) ) {
		return NULL;
	}

	if( passwd->bv_len >= scheme->name.bv_len ) {
		if( strncasecmp( passwd->bv_val, scheme->name.bv_val, scheme->name.bv_len ) == 0 ) {
			bv->bv_val = &passwd->bv_val[scheme->name.bv_len];
			bv->bv_len = passwd->bv_len - scheme->name.bv_len;

			return bv;
		}
	}

	return NULL;
}

/*
 * Return 0 if creds are good.
 */
int
lutil_passwd(
	const struct berval *passwd,	/* stored passwd */
	const struct berval *cred,		/* user cred */
	const char **schemes,
	const char **text )
{
	struct pw_slist *pws;

	if ( text ) *text = NULL;

	if (cred == NULL || cred->bv_len == 0 ||
		passwd == NULL || passwd->bv_len == 0 )
	{
		return -1;
	}

	if (!pw_inited) lutil_passwd_init();

	for( pws=pw_schemes; pws; pws=pws->next ) {
		if( pws->s.chk_fn ) {
			struct berval x;
			struct berval *p = passwd_scheme( &(pws->s),
				passwd, &x, schemes );

			if( p != NULL ) {
				return (pws->s.chk_fn)( &(pws->s.name), p, cred, text );
			}
		}
	}

#ifdef SLAPD_CLEARTEXT
	/* Do we think there is a scheme specifier here that we
	 * didn't recognize? Assume a scheme name is at least 1 character.
	 */
	if (( passwd->bv_val[0] == '{' ) &&
		( ber_bvchr( passwd, '}' ) > passwd->bv_val+1 ))
	{
		return 1;
	}
	if( is_allowed_scheme("{CLEARTEXT}", schemes ) ) {
		return ( passwd->bv_len == cred->bv_len ) ?
			memcmp( passwd->bv_val, cred->bv_val, passwd->bv_len )
			: 1;
	}
#endif
	return 1;
}

int lutil_passwd_generate( struct berval *pw, ber_len_t len )
{

	if( len < 1 ) return -1;

	pw->bv_len = len;
	pw->bv_val = ber_memalloc( len + 1 );

	if( pw->bv_val == NULL ) {
		return -1;
	}

	if( lutil_entropy( (unsigned char *) pw->bv_val, pw->bv_len) < 0 ) {
		return -1; 
	}

	for( len = 0; len < pw->bv_len; len++ ) {
		pw->bv_val[len] = crypt64[
			pw->bv_val[len] % (sizeof(crypt64)-1) ];
	}

	pw->bv_val[len] = '\0';
	
	return 0;
}

int lutil_passwd_hash(
	const struct berval * passwd,
	const char * method,
	struct berval *hash,
	const char **text )
{
	const struct pw_scheme *sc = get_scheme( method );

	hash->bv_val = NULL;
	hash->bv_len = 0;

	if( sc == NULL ) {
		if( text ) *text = "scheme not recognized";
		return -1;
	}

	if( ! sc->hash_fn ) {
		if( text ) *text = "scheme provided no hash function";
		return -1;
	}

	if( text ) *text = NULL;

	return (sc->hash_fn)( &sc->name, passwd, hash, text );
}

/* pw_string is only called when SLAPD_CRYPT is defined */
#if defined(SLAPD_CRYPT)
static int pw_string(
	const struct berval *sc,
	struct berval *passwd )
{
	struct berval pw;

	pw.bv_len = sc->bv_len + passwd->bv_len;
	pw.bv_val = ber_memalloc( pw.bv_len + 1 );

	if( pw.bv_val == NULL ) {
		return LUTIL_PASSWD_ERR;
	}

	AC_MEMCPY( pw.bv_val, sc->bv_val, sc->bv_len );
	AC_MEMCPY( &pw.bv_val[sc->bv_len], passwd->bv_val, passwd->bv_len );

	pw.bv_val[pw.bv_len] = '\0';
	*passwd = pw;

	return LUTIL_PASSWD_OK;
}
#endif /* SLAPD_CRYPT */

int lutil_passwd_string64(
	const struct berval *sc,
	const struct berval *hash,
	struct berval *b64,
	const struct berval *salt )
{
	int rc;
	struct berval string;
	size_t b64len;

	if( salt ) {
		/* need to base64 combined string */
		string.bv_len = hash->bv_len + salt->bv_len;
		string.bv_val = ber_memalloc( string.bv_len + 1 );

		if( string.bv_val == NULL ) {
			return LUTIL_PASSWD_ERR;
		}

		AC_MEMCPY( string.bv_val, hash->bv_val,
			hash->bv_len );
		AC_MEMCPY( &string.bv_val[hash->bv_len], salt->bv_val,
			salt->bv_len );
		string.bv_val[string.bv_len] = '\0';

	} else {
		string = *hash;
	}

	b64len = LUTIL_BASE64_ENCODE_LEN( string.bv_len ) + 1;
	b64->bv_len = b64len + sc->bv_len;
	b64->bv_val = ber_memalloc( b64->bv_len + 1 );

	if( b64->bv_val == NULL ) {
		if( salt ) ber_memfree( string.bv_val );
		return LUTIL_PASSWD_ERR;
	}

	AC_MEMCPY(b64->bv_val, sc->bv_val, sc->bv_len);

	rc = lutil_b64_ntop(
		(unsigned char *) string.bv_val, string.bv_len,
		&b64->bv_val[sc->bv_len], b64len );

	if( salt ) ber_memfree( string.bv_val );
	
	if( rc < 0 ) {
		return LUTIL_PASSWD_ERR;
	}

	/* recompute length */
	b64->bv_len = sc->bv_len + rc;
	assert( strlen(b64->bv_val) == b64->bv_len );
	return LUTIL_PASSWD_OK;
}

/* PASSWORD CHECK ROUTINES */

#ifdef LUTIL_SHA1_BYTES
static int chk_ssha1(
	const struct berval *sc,
	const struct berval * passwd,
	const struct berval * cred,
	const char **text )
{
	lutil_SHA1_CTX SHA1context;
	unsigned char SHA1digest[LUTIL_SHA1_BYTES];
	int rc;
	unsigned char *orig_pass = NULL;
	size_t decode_len = LUTIL_BASE64_DECODE_LEN(passwd->bv_len);

	/* safety check -- must have some salt */
	if (decode_len <= sizeof(SHA1digest)) {
		return LUTIL_PASSWD_ERR;
	}

	/* decode base64 password */
	orig_pass = (unsigned char *) ber_memalloc(decode_len + 1);

	if( orig_pass == NULL ) return LUTIL_PASSWD_ERR;

	rc = lutil_b64_pton(passwd->bv_val, orig_pass, decode_len);

	/* safety check -- must have some salt */
	if (rc <= (int)(sizeof(SHA1digest))) {
		ber_memfree(orig_pass);
		return LUTIL_PASSWD_ERR;
	}
 
	/* hash credentials with salt */
	lutil_SHA1Init(&SHA1context);
	lutil_SHA1Update(&SHA1context,
		(const unsigned char *) cred->bv_val, cred->bv_len);
	lutil_SHA1Update(&SHA1context,
		(const unsigned char *) &orig_pass[sizeof(SHA1digest)],
		rc - sizeof(SHA1digest));
	lutil_SHA1Final(SHA1digest, &SHA1context);
 
	/* compare */
	rc = memcmp((char *)orig_pass, (char *)SHA1digest, sizeof(SHA1digest));
	ber_memfree(orig_pass);
	return rc ? LUTIL_PASSWD_ERR : LUTIL_PASSWD_OK;
}

static int chk_sha1(
	const struct berval *sc,
	const struct berval * passwd,
	const struct berval * cred,
	const char **text )
{
	lutil_SHA1_CTX SHA1context;
	unsigned char SHA1digest[LUTIL_SHA1_BYTES];
	int rc;
	unsigned char *orig_pass = NULL;
	size_t decode_len = LUTIL_BASE64_DECODE_LEN(passwd->bv_len);
 
	/* safety check */
	if (decode_len < sizeof(SHA1digest)) {
		return LUTIL_PASSWD_ERR;
	}

	/* base64 un-encode password */
	orig_pass = (unsigned char *) ber_memalloc(decode_len + 1);

	if( orig_pass == NULL ) return LUTIL_PASSWD_ERR;

	rc = lutil_b64_pton(passwd->bv_val, orig_pass, decode_len);

	if( rc != sizeof(SHA1digest) ) {
		ber_memfree(orig_pass);
		return LUTIL_PASSWD_ERR;
	}
 
	/* hash credentials with salt */
	lutil_SHA1Init(&SHA1context);
	lutil_SHA1Update(&SHA1context,
		(const unsigned char *) cred->bv_val, cred->bv_len);
	lutil_SHA1Final(SHA1digest, &SHA1context);
 
	/* compare */
	rc = memcmp((char *)orig_pass, (char *)SHA1digest, sizeof(SHA1digest));
	ber_memfree(orig_pass);
	return rc ? LUTIL_PASSWD_ERR : LUTIL_PASSWD_OK;
}
#endif

static int chk_smd5(
	const struct berval *sc,
	const struct berval * passwd,
	const struct berval * cred,
	const char **text )
{
	lutil_MD5_CTX MD5context;
	unsigned char MD5digest[LUTIL_MD5_BYTES];
	int rc;
	unsigned char *orig_pass = NULL;
	size_t decode_len = LUTIL_BASE64_DECODE_LEN(passwd->bv_len);

	/* safety check */
	if (decode_len <= sizeof(MD5digest)) {
		return LUTIL_PASSWD_ERR;
	}

	/* base64 un-encode password */
	orig_pass = (unsigned char *) ber_memalloc(decode_len + 1);

	if( orig_pass == NULL ) return LUTIL_PASSWD_ERR;

	rc = lutil_b64_pton(passwd->bv_val, orig_pass, decode_len);

	if (rc <= (int)(sizeof(MD5digest))) {
		ber_memfree(orig_pass);
		return LUTIL_PASSWD_ERR;
	}

	/* hash credentials with salt */
	lutil_MD5Init(&MD5context);
	lutil_MD5Update(&MD5context,
		(const unsigned char *) cred->bv_val,
		cred->bv_len );
	lutil_MD5Update(&MD5context,
		&orig_pass[sizeof(MD5digest)],
		rc - sizeof(MD5digest));
	lutil_MD5Final(MD5digest, &MD5context);

	/* compare */
	rc = memcmp((char *)orig_pass, (char *)MD5digest, sizeof(MD5digest));
	ber_memfree(orig_pass);
	return rc ? LUTIL_PASSWD_ERR : LUTIL_PASSWD_OK;
}

static int chk_md5(
	const struct berval *sc,
	const struct berval * passwd,
	const struct berval * cred,
	const char **text )
{
	lutil_MD5_CTX MD5context;
	unsigned char MD5digest[LUTIL_MD5_BYTES];
	int rc;
	unsigned char *orig_pass = NULL;
	size_t decode_len = LUTIL_BASE64_DECODE_LEN(passwd->bv_len);

	/* safety check */
	if (decode_len < sizeof(MD5digest)) {
		return LUTIL_PASSWD_ERR;
	}

	/* base64 un-encode password */
	orig_pass = (unsigned char *) ber_memalloc(decode_len + 1);

	if( orig_pass == NULL ) return LUTIL_PASSWD_ERR;

	rc = lutil_b64_pton(passwd->bv_val, orig_pass, decode_len);
	if ( rc != sizeof(MD5digest) ) {
		ber_memfree(orig_pass);
		return LUTIL_PASSWD_ERR;
	}

	/* hash credentials with salt */
	lutil_MD5Init(&MD5context);
	lutil_MD5Update(&MD5context,
		(const unsigned char *) cred->bv_val,
		cred->bv_len );
	lutil_MD5Final(MD5digest, &MD5context);

	/* compare */
	rc = memcmp((char *)orig_pass, (char *)MD5digest, sizeof(MD5digest));
	ber_memfree(orig_pass);
	return rc ? LUTIL_PASSWD_ERR : LUTIL_PASSWD_OK;
}

#ifdef SLAPD_CRYPT
static int lutil_crypt(
	const char *key,
	const char *salt,
	char **hash )
{
	char *cr = crypt( key, salt );
	int rc;

	if( cr == NULL || cr[0] == '\0' ) {
		/* salt must have been invalid */
		rc = LUTIL_PASSWD_ERR;
	} else {
		if ( hash ) {
			*hash = ber_strdup( cr );
			rc = LUTIL_PASSWD_OK;
		} else {
			rc = strcmp( salt, cr ) ? LUTIL_PASSWD_ERR : LUTIL_PASSWD_OK;
		}
	}
	return rc;
}

static int chk_crypt(
	const struct berval *sc,
	const struct berval * passwd,
	const struct berval * cred,
	const char **text )
{
	unsigned int i;

	for( i=0; i<cred->bv_len; i++) {
		if(cred->bv_val[i] == '\0') {
			return LUTIL_PASSWD_ERR;	/* NUL character in password */
		}
	}

	if( cred->bv_val[i] != '\0' ) {
		return LUTIL_PASSWD_ERR;	/* cred must behave like a string */
	}

	if( passwd->bv_len < 2 ) {
		return LUTIL_PASSWD_ERR;	/* passwd must be at least two characters long */
	}

	for( i=0; i<passwd->bv_len; i++) {
		if(passwd->bv_val[i] == '\0') {
			return LUTIL_PASSWD_ERR;	/* NUL character in password */
		}
	}

	if( passwd->bv_val[i] != '\0' ) {
		return LUTIL_PASSWD_ERR;	/* passwd must behave like a string */
	}

	return lutil_cryptptr( cred->bv_val, passwd->bv_val, NULL );
}

# if defined( HAVE_GETPWNAM ) && defined( HAVE_STRUCT_PASSWD_PW_PASSWD )
static int chk_unix(
	const struct berval *sc,
	const struct berval * passwd,
	const struct berval * cred,
	const char **text )
{
	unsigned int i;
	char *pw;

	for( i=0; i<cred->bv_len; i++) {
		if(cred->bv_val[i] == '\0') {
			return LUTIL_PASSWD_ERR;	/* NUL character in password */
		}
	}
	if( cred->bv_val[i] != '\0' ) {
		return LUTIL_PASSWD_ERR;	/* cred must behave like a string */
	}

	for( i=0; i<passwd->bv_len; i++) {
		if(passwd->bv_val[i] == '\0') {
			return LUTIL_PASSWD_ERR;	/* NUL character in password */
		}
	}

	if( passwd->bv_val[i] != '\0' ) {
		return LUTIL_PASSWD_ERR;	/* passwd must behave like a string */
	}

	{
		struct passwd *pwd = getpwnam(passwd->bv_val);

		if(pwd == NULL) {
			return LUTIL_PASSWD_ERR;	/* not found */
		}

		pw = pwd->pw_passwd;
	}
#  ifdef HAVE_GETSPNAM
	{
		struct spwd *spwd = getspnam(passwd->bv_val);

		if(spwd != NULL) {
			pw = spwd->sp_pwdp;
		}
	}
#  endif
#  ifdef HAVE_AIX_SECURITY
	{
		struct userpw *upw = getuserpw(passwd->bv_val);

		if (upw != NULL) {
			pw = upw->upw_passwd;
		}
	}
#  endif

	if( pw == NULL || pw[0] == '\0' || pw[1] == '\0' ) {
		/* password must must be at least two characters long */
		return LUTIL_PASSWD_ERR;
	}

	return lutil_cryptptr( cred->bv_val, pw, NULL );
}
# endif
#endif

/* PASSWORD GENERATION ROUTINES */

#ifdef LUTIL_SHA1_BYTES
static int hash_ssha1(
	const struct berval *scheme,
	const struct berval  *passwd,
	struct berval *hash,
	const char **text )
{
	lutil_SHA1_CTX  SHA1context;
	unsigned char   SHA1digest[LUTIL_SHA1_BYTES];
	char            saltdata[SALT_SIZE];
	struct berval digest;
	struct berval salt;

	digest.bv_val = (char *) SHA1digest;
	digest.bv_len = sizeof(SHA1digest);
	salt.bv_val = saltdata;
	salt.bv_len = sizeof(saltdata);

	if( lutil_entropy( (unsigned char *) salt.bv_val, salt.bv_len) < 0 ) {
		return LUTIL_PASSWD_ERR; 
	}

	lutil_SHA1Init( &SHA1context );
	lutil_SHA1Update( &SHA1context,
		(const unsigned char *)passwd->bv_val, passwd->bv_len );
	lutil_SHA1Update( &SHA1context,
		(const unsigned char *)salt.bv_val, salt.bv_len );
	lutil_SHA1Final( SHA1digest, &SHA1context );

	return lutil_passwd_string64( scheme, &digest, hash, &salt);
}

static int hash_sha1(
	const struct berval *scheme,
	const struct berval  *passwd,
	struct berval *hash,
	const char **text )
{
	lutil_SHA1_CTX  SHA1context;
	unsigned char   SHA1digest[LUTIL_SHA1_BYTES];
	struct berval digest;
	digest.bv_val = (char *) SHA1digest;
	digest.bv_len = sizeof(SHA1digest);
     
	lutil_SHA1Init( &SHA1context );
	lutil_SHA1Update( &SHA1context,
		(const unsigned char *)passwd->bv_val, passwd->bv_len );
	lutil_SHA1Final( SHA1digest, &SHA1context );
            
	return lutil_passwd_string64( scheme, &digest, hash, NULL);
}
#endif

static int hash_smd5(
	const struct berval *scheme,
	const struct berval  *passwd,
	struct berval *hash,
	const char **text )
{
	lutil_MD5_CTX   MD5context;
	unsigned char   MD5digest[LUTIL_MD5_BYTES];
	char            saltdata[SALT_SIZE];
	struct berval digest;
	struct berval salt;

	digest.bv_val = (char *) MD5digest;
	digest.bv_len = sizeof(MD5digest);
	salt.bv_val = saltdata;
	salt.bv_len = sizeof(saltdata);

	if( lutil_entropy( (unsigned char *) salt.bv_val, salt.bv_len) < 0 ) {
		return LUTIL_PASSWD_ERR; 
	}

	lutil_MD5Init( &MD5context );
	lutil_MD5Update( &MD5context,
		(const unsigned char *) passwd->bv_val, passwd->bv_len );
	lutil_MD5Update( &MD5context,
		(const unsigned char *) salt.bv_val, salt.bv_len );
	lutil_MD5Final( MD5digest, &MD5context );

	return lutil_passwd_string64( scheme, &digest, hash, &salt );
}

static int hash_md5(
	const struct berval *scheme,
	const struct berval  *passwd,
	struct berval *hash,
	const char **text )
{
	lutil_MD5_CTX   MD5context;
	unsigned char   MD5digest[LUTIL_MD5_BYTES];

	struct berval digest;

	digest.bv_val = (char *) MD5digest;
	digest.bv_len = sizeof(MD5digest);

	lutil_MD5Init( &MD5context );
	lutil_MD5Update( &MD5context,
		(const unsigned char *) passwd->bv_val, passwd->bv_len );
	lutil_MD5Final( MD5digest, &MD5context );

	return lutil_passwd_string64( scheme, &digest, hash, NULL );
;
}

#ifdef SLAPD_CRYPT
static int hash_crypt(
	const struct berval *scheme,
	const struct berval *passwd,
	struct berval *hash,
	const char **text )
{
	unsigned char salt[32];	/* salt suitable for most anything */
	unsigned int i;
	char *save;
	int rc;

	for( i=0; i<passwd->bv_len; i++) {
		if(passwd->bv_val[i] == '\0') {
			return LUTIL_PASSWD_ERR;	/* NUL character in password */
		}
	}

	if( passwd->bv_val[i] != '\0' ) {
		return LUTIL_PASSWD_ERR;	/* passwd must behave like a string */
	}

	if( lutil_entropy( salt, sizeof( salt ) ) < 0 ) {
		return LUTIL_PASSWD_ERR; 
	}

	for( i=0; i< ( sizeof(salt) - 1 ); i++ ) {
		salt[i] = crypt64[ salt[i] % (sizeof(crypt64)-1) ];
	}
	salt[sizeof( salt ) - 1 ] = '\0';

	if( salt_format != NULL ) {
		/* copy the salt we made into entropy before snprintfing
		   it back into the salt */
		char entropy[sizeof(salt)];
		strcpy( entropy, (char *) salt );
		snprintf( (char *) salt, sizeof(entropy), salt_format, entropy );
	}

	rc = lutil_cryptptr( passwd->bv_val, (char *) salt, &hash->bv_val );
	if ( rc != LUTIL_PASSWD_OK ) return rc;

	if( hash->bv_val == NULL ) return -1;

	hash->bv_len = strlen( hash->bv_val );

	save = hash->bv_val;

	if( hash->bv_len == 0 ) {
		rc = LUTIL_PASSWD_ERR;
	} else {
		rc = pw_string( scheme, hash );
	}
	ber_memfree( save );
	return rc;
}
#endif

int lutil_salt_format(const char *format)
{
#ifdef SLAPD_CRYPT
	ber_memfree( salt_format );

	salt_format = format != NULL ? ber_strdup( format ) : NULL;
#endif

	return 0;
}

#ifdef SLAPD_CLEARTEXT
static int hash_clear(
	const struct berval *scheme,
	const struct berval  *passwd,
	struct berval *hash,
	const char **text )
{
	ber_dupbv( hash, (struct berval *)passwd );
	return LUTIL_PASSWD_OK;
}
#endif

