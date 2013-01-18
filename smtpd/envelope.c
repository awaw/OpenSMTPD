/*	$OpenBSD: envelope.c,v 1.17 2012/10/12 08:51:02 eric Exp $	*/

/*
 * Copyright (c) 2011 Gilles Chehade <gilles@poolp.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "includes.h"

#include <sys/types.h>
#include "sys-queue.h"
#include "sys-tree.h"
#include <sys/param.h>
#include <sys/socket.h>
#include <sys/stat.h>

#include <netinet/in.h>
#include <arpa/inet.h>

#include <ctype.h>
#include <err.h>
#include <errno.h>
#include <event.h>
#include <fcntl.h>
#include "imsg.h"
#include <inttypes.h>
#include <libgen.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "smtpd.h"
#include "log.h"

static int ascii_load_uint16(uint16_t *, char *);
static int ascii_load_uint32(uint32_t *, char *);
static int ascii_load_time(time_t *, char *);
static int ascii_load_type(enum delivery_type *, char *);
static int ascii_load_string(char *, char *, size_t);
static int ascii_load_sockaddr(struct sockaddr_storage *, char *);
static int ascii_load_mda_method(enum action_type *, char *);
static int ascii_load_mailaddr(struct mailaddr *, char *);
static int ascii_load_flags(enum envelope_flags *, char *);
static int ascii_load_mta_relay_url(struct relayhost *, char *);
static int ascii_load_bounce_type(enum bounce_type *, char *);

static int ascii_dump_uint16(uint16_t, char *, size_t);
static int ascii_dump_uint32(uint32_t, char *, size_t);
static int ascii_dump_time(time_t, char *, size_t);
static int ascii_dump_string(const char *, char *, size_t);
static int ascii_dump_type(enum delivery_type, char *, size_t);
static int ascii_dump_mda_method(enum action_type, char *, size_t);
static int ascii_dump_mailaddr(struct mailaddr *, char *, size_t);
static int ascii_dump_flags(enum envelope_flags, char *, size_t);
static int ascii_dump_mta_relay_url(struct relayhost *, char *, size_t);
static int ascii_dump_bounce_type(enum bounce_type, char *, size_t);

void
envelope_set_errormsg(struct envelope *e, char *fmt, ...)
{
	int ret;
	va_list ap;

	va_start(ap, fmt);
	ret = vsnprintf(e->errorline, sizeof(e->errorline), fmt, ap);
	va_end(ap);

	/* this should not happen */
	if (ret == -1)
		err(1, "vsnprintf");

	if ((size_t)ret >= sizeof(e->errorline))
		strlcpy(e->errorline + (sizeof(e->errorline) - 4), "...", 4);
}

int
envelope_load_buffer(struct envelope *ep, char *buf, size_t buflen)
{
	enum envelope_field fields[] = {
		EVP_VERSION,
		EVP_MSGID,
		EVP_HOSTNAME,
		EVP_SOCKADDR,
		EVP_HELO,
		EVP_SENDER,
		EVP_RCPT,
		EVP_DEST,
		EVP_TYPE,
		EVP_CTIME,
		EVP_EXPIRE,
		EVP_RETRY,
		EVP_LASTTRY,
		EVP_LASTBOUNCE,
		EVP_FLAGS,
		EVP_ERRORLINE,
		EVP_MDA_METHOD,
		EVP_MDA_USERTABLE,
		EVP_MDA_BUFFER,
		EVP_MDA_USER,
		EVP_MTA_RELAY_SOURCE,
		EVP_MTA_RELAY_CERT,
		EVP_MTA_RELAY_AUTH,
		EVP_MTA_RELAY_HELO,
		EVP_MTA_RELAY,
		EVP_BOUNCE_TYPE,
		EVP_BOUNCE_DELAY,
		EVP_BOUNCE_EXPIRE,
	};
	char	*field, *nextline;
	size_t	 len;
	int	 i;
	int	 n;
	int	 ret;

	n = sizeof(fields) / sizeof(enum envelope_field);
	bzero(ep, sizeof (*ep));

	while (buflen > 0) {
		len = strcspn(buf, "\n");
		buf[len] = '\0';
		nextline = buf + len + 1;
		buflen -= (nextline - buf);
		for (i = 0; i < n; ++i) {
			field = envelope_ascii_field_name(fields[i]);
			len = strlen(field);
			if (! strncasecmp(field, buf, len)) {
				/* skip kw and tailing whitespaces */
				buf += len;
				while (*buf && isspace(*buf))
					buf++;

				/* we *want* ':' */
				if (*buf != ':')
					continue;
				buf++;

				/* skip whitespaces after separator */
				while (*buf && isspace(*buf))
				    buf++;

				ret = envelope_ascii_load(fields[i], ep, buf);
				if (ret == 0)
					goto err;
				buf = nextline;
				break;
			}
		}

		/* unknown keyword */
		if (i == n)
			goto err;
	}
	return (1);

err:
	return (0);
}

int
envelope_dump_buffer(struct envelope *ep, char *dest, size_t len)
{
	char	buf[8192];

	enum envelope_field fields[] = {
		EVP_VERSION,
		EVP_TYPE,
		EVP_HELO,
		EVP_HOSTNAME,
		EVP_ERRORLINE,
		EVP_SOCKADDR,
		EVP_SENDER,
		EVP_RCPT,
		EVP_DEST,
		EVP_CTIME,
		EVP_LASTTRY,
		EVP_LASTBOUNCE,
		EVP_EXPIRE,
		EVP_RETRY,
		EVP_FLAGS
	};
	enum envelope_field mda_fields[] = {
		EVP_MDA_METHOD,
		EVP_MDA_USERTABLE,
		EVP_MDA_BUFFER,
		EVP_MDA_USER
	};
	enum envelope_field mta_fields[] = {
		EVP_MTA_RELAY_SOURCE,
		EVP_MTA_RELAY_CERT,
		EVP_MTA_RELAY_AUTH,
		EVP_MTA_RELAY_HELO,
		EVP_MTA_RELAY,
	};
	enum envelope_field bounce_fields[] = {
		EVP_BOUNCE_TYPE,
		EVP_BOUNCE_DELAY,
		EVP_BOUNCE_EXPIRE,
	};
	enum envelope_field *pfields = NULL;
	int	 i, n, l;
	char	*p;

	p = dest;
	n = sizeof(fields) / sizeof(enum envelope_field);
	for (i = 0; i < n; ++i) {
		bzero(buf, sizeof buf);
		if (! envelope_ascii_dump(fields[i], ep, buf, sizeof buf))
			goto err;
		if (buf[0] == '\0')
			continue;

		l = snprintf(dest, len, "%s: %s\n",
			envelope_ascii_field_name(fields[i]), buf);
		if (l == -1 || (size_t) l >= len)
			goto err;
		dest += l;
		len -= l;
	}

	switch (ep->type) {
	case D_MDA:
		pfields = mda_fields;
		n = sizeof(mda_fields) / sizeof(enum envelope_field);
		break;
	case D_MTA:
		pfields = mta_fields;
		n = sizeof(mta_fields) / sizeof(enum envelope_field);
		break;
	case D_BOUNCE:
		pfields = bounce_fields;
		n = sizeof(bounce_fields) / sizeof(enum envelope_field);
		break;
	default:
		goto err;
	}

	if (pfields) {
		for (i = 0; i < n; ++i) {
			bzero(buf, sizeof buf);
			if (! envelope_ascii_dump(pfields[i], ep, buf,
				sizeof buf))
				goto err;
			if (buf[0] == '\0')
				continue;

			l = snprintf(dest, len, "%s: %s\n",
				envelope_ascii_field_name(pfields[i]), buf);
			if (l == -1 || (size_t) l >= len)
				goto err;
			dest += l;
			len -= l;
		}
	}

	return (dest - p);

err:
	return (0);
}

char *
envelope_ascii_field_name(enum envelope_field field)
{
	switch (field) {
	case EVP_VERSION:
		return "version";
	case EVP_MSGID:
		return "msgid";
	case EVP_TYPE:
		return "type";
	case EVP_HELO:
		return "helo";
	case EVP_HOSTNAME:
		return "hostname";
	case EVP_ERRORLINE:
		return "errorline";
	case EVP_SOCKADDR:
		return "sockaddr";
	case EVP_SENDER:
		return "sender";
	case EVP_RCPT:
		return "rcpt";
	case EVP_DEST:
		return "dest";
	case EVP_CTIME:
		return "ctime";
	case EVP_EXPIRE:
		return "expire";
	case EVP_RETRY:
		return "retry";
	case EVP_LASTTRY:
		return "last-try";
	case EVP_LASTBOUNCE:
		return "last-bounce";
	case EVP_FLAGS:
		return "flags";
	case EVP_MDA_METHOD:
		return "mda-method";
	case EVP_MDA_BUFFER:
		return "mda-buffer";
	case EVP_MDA_USER:
		return "mda-user";
	case EVP_MDA_USERTABLE:
		return "mda-usertable";
	case EVP_MTA_RELAY:
		return "mta-relay";
	case EVP_MTA_RELAY_AUTH:
		return "mta-relay-auth";
	case EVP_MTA_RELAY_CERT:
		return "mta-relay-cert";
	case EVP_MTA_RELAY_SOURCE:
		return "mta-relay-source";
	case EVP_MTA_RELAY_HELO:
		return "mta-relay-helo";
	case EVP_BOUNCE_TYPE:
		return "bounce-type";
	case EVP_BOUNCE_DELAY:
		return "bounce-delay";
	case EVP_BOUNCE_EXPIRE:
		return "bounce-expire";
	}

	return NULL;
}

int
envelope_ascii_load(enum envelope_field field, struct envelope *ep, char *buf)
{
	switch (field) {
	case EVP_VERSION:
		return ascii_load_uint32(&ep->version, buf);
	case EVP_MSGID:
		return 1;
	case EVP_TYPE:
		return ascii_load_type(&ep->type, buf);
	case EVP_HELO:
		return ascii_load_string(ep->helo, buf, sizeof ep->helo);
	case EVP_HOSTNAME:
		return ascii_load_string(ep->hostname, buf,
		    sizeof ep->hostname);
	case EVP_ERRORLINE:
		return ascii_load_string(ep->errorline, buf,
		    sizeof ep->errorline);
	case EVP_SOCKADDR:
		return ascii_load_sockaddr(&ep->ss, buf);
	case EVP_SENDER:
		return ascii_load_mailaddr(&ep->sender, buf);
	case EVP_RCPT:
		return ascii_load_mailaddr(&ep->rcpt, buf);
	case EVP_DEST:
		return ascii_load_mailaddr(&ep->dest, buf);
	case EVP_MDA_METHOD:
		return ascii_load_mda_method(&ep->agent.mda.method, buf);
	case EVP_MDA_BUFFER:
		return ascii_load_string(ep->agent.mda.buffer, buf,
		    sizeof ep->agent.mda.buffer);
	case EVP_MDA_USER:
		return ascii_load_string(ep->agent.mda.username, buf,
		    sizeof ep->agent.mda.username);
	case EVP_MDA_USERTABLE:
		return ascii_load_string(ep->agent.mda.usertable, buf,
		    sizeof ep->agent.mda.usertable);
	case EVP_MTA_RELAY_SOURCE:
		return ascii_load_string(ep->agent.mta.relay.sourcetable, buf,
		    sizeof ep->agent.mta.relay.sourcetable);
	case EVP_MTA_RELAY_CERT:
		return ascii_load_string(ep->agent.mta.relay.cert, buf,
		    sizeof ep->agent.mta.relay.cert);
	case EVP_MTA_RELAY_AUTH:
		return ascii_load_string(ep->agent.mta.relay.authtable, buf,
		    sizeof ep->agent.mta.relay.authtable);
	case EVP_MTA_RELAY_HELO:
		return ascii_load_string(ep->agent.mta.relay.helotable, buf,
		    sizeof ep->agent.mta.relay.helotable);
	case EVP_MTA_RELAY:
		return ascii_load_mta_relay_url(&ep->agent.mta.relay, buf);
	case EVP_CTIME:
		return ascii_load_time(&ep->creation, buf);
	case EVP_EXPIRE:
		return ascii_load_time(&ep->expire, buf);
	case EVP_RETRY:
		return ascii_load_uint16(&ep->retry, buf);
	case EVP_LASTTRY:
		return ascii_load_time(&ep->lasttry, buf);
	case EVP_LASTBOUNCE:
		return ascii_load_time(&ep->lastbounce, buf);
	case EVP_FLAGS:
		return ascii_load_flags(&ep->flags, buf);
	case EVP_BOUNCE_TYPE:
		return ascii_load_bounce_type(&ep->agent.bounce.type, buf);
	case EVP_BOUNCE_DELAY:
		return ascii_load_time(&ep->agent.bounce.delay, buf);
	case EVP_BOUNCE_EXPIRE:
		return ascii_load_time(&ep->agent.bounce.expire, buf);
	}
	return 0;
}

int
envelope_ascii_dump(enum envelope_field field, struct envelope *ep,
    char *buf, size_t len)
{
	switch (field) {
	case EVP_VERSION:
		return ascii_dump_uint32(SMTPD_ENVELOPE_VERSION, buf, len);
	case EVP_MSGID:
		return 1;
	case EVP_TYPE:
		return ascii_dump_type(ep->type, buf, len);
	case EVP_HELO:
		return ascii_dump_string(ep->helo, buf, len);
	case EVP_HOSTNAME:
		return ascii_dump_string(ep->hostname, buf, len);
	case EVP_ERRORLINE:
		return ascii_dump_string(ep->errorline, buf, len);
	case EVP_SOCKADDR:
		return ascii_dump_string(ss_to_text(&ep->ss), buf, len);
	case EVP_SENDER:
		return ascii_dump_mailaddr(&ep->sender, buf, len);
	case EVP_RCPT:
		return ascii_dump_mailaddr(&ep->rcpt, buf, len);
	case EVP_DEST:
		return ascii_dump_mailaddr(&ep->dest, buf, len);
	case EVP_MDA_METHOD:
		return ascii_dump_mda_method(ep->agent.mda.method, buf, len);
	case EVP_MDA_BUFFER:
		return ascii_dump_string(ep->agent.mda.buffer, buf, len);
	case EVP_MDA_USER:
		return ascii_dump_string(ep->agent.mda.username, buf, len);
	case EVP_MDA_USERTABLE:
		return ascii_dump_string(ep->agent.mda.usertable, buf, len);
	case EVP_MTA_RELAY_SOURCE:
		return ascii_dump_string(ep->agent.mta.relay.sourcetable,
		    buf, len);
	case EVP_MTA_RELAY_CERT:
		return ascii_dump_string(ep->agent.mta.relay.cert,
		    buf, len);
	case EVP_MTA_RELAY_AUTH:
		return ascii_dump_string(ep->agent.mta.relay.authtable,
		    buf, len);
	case EVP_MTA_RELAY_HELO:
		return ascii_dump_string(ep->agent.mta.relay.helotable,
		    buf, len);
	case EVP_MTA_RELAY:
		if (ep->agent.mta.relay.hostname[0])
			return ascii_dump_mta_relay_url(&ep->agent.mta.relay, buf, len);
		return 1;
	case EVP_CTIME:
		return ascii_dump_time(ep->creation, buf, len);
	case EVP_EXPIRE:
		return ascii_dump_time(ep->expire, buf, len);
	case EVP_RETRY:
		return ascii_dump_uint16(ep->retry, buf, len);
	case EVP_LASTTRY:
		return ascii_dump_time(ep->lasttry, buf, len);
	case EVP_LASTBOUNCE:
		return ascii_dump_time(ep->lastbounce, buf, len);
	case EVP_FLAGS:
		return ascii_dump_flags(ep->flags, buf, len);
	case EVP_BOUNCE_TYPE:
		return ascii_dump_bounce_type(ep->agent.bounce.type, buf, len);
	case EVP_BOUNCE_DELAY:
		if (ep->agent.bounce.type != B_WARNING)
			return (1);
		return ascii_dump_time(ep->agent.bounce.delay, buf, len);
	case EVP_BOUNCE_EXPIRE:
		if (ep->agent.bounce.type != B_WARNING)
			return (1);
		return ascii_dump_time(ep->agent.bounce.expire, buf, len);
	}
	return 0;
}

static int
ascii_load_uint16(uint16_t *dest, char *buf)
{
	const char *errstr;

	*dest = strtonum(buf, 0, 0xffff, &errstr);
	if (errstr)
		return 0;
	return 1;
}

static int
ascii_load_uint32(uint32_t *dest, char *buf)
{
	const char *errstr;

	*dest = strtonum(buf, 0, 0xffffffff, &errstr);
	if (errstr)
		return 0;
	return 1;
}

static int
ascii_load_time(time_t *dest, char *buf)
{
	const char *errstr;

	*dest = (time_t) strtonum(buf, 0, 0x7fffffff, &errstr);
	if (errstr)
		return 0;
	return 1;
}

static int
ascii_load_type(enum delivery_type *dest, char *buf)
{
	if (strcasecmp(buf, "mda") == 0)
		*dest = D_MDA;
	else if (strcasecmp(buf, "mta") == 0)
		*dest = D_MTA;
	else if (strcasecmp(buf, "bounce") == 0)
		*dest = D_BOUNCE;
	else
		return 0;
	return 1;
}

static int
ascii_load_string(char *dest, char *buf, size_t len)
{
	if (strlcpy(dest, buf, len) >= len)
		return 0;
	return 1;
}

static int
ascii_load_sockaddr(struct sockaddr_storage *ss, char *buf)
{
	struct sockaddr_in6 ssin6;
	struct sockaddr_in  ssin;

	if (!strcmp("local", buf)) {
		ss->ss_family = AF_LOCAL;
	}
	else if (strncasecmp("IPv6:", buf, 5) == 0) {
		if (inet_pton(AF_INET6, buf + 5, &ssin6.sin6_addr) != 1)
			return 0;
		ssin6.sin6_family = AF_INET6;
		memcpy(ss, &ssin6, sizeof(ssin6));
#ifdef HAVE_STRUCT_SOCKADDR_STORAGE_SS_LEN
		ss->ss_len = sizeof(struct sockaddr_in6);
#endif
	}
	else {
		if (inet_pton(AF_INET, buf, &ssin.sin_addr) != 1)
			return 0;
		ssin.sin_family = AF_INET;
		memcpy(ss, &ssin, sizeof(ssin));
#ifdef HAVE_STRUCT_SOCKADDR_STORAGE_SS_LEN
		ss->ss_len = sizeof(struct sockaddr_in);
#endif
	}
	return 1;
}

static int
ascii_load_mda_method(enum action_type *dest, char *buf)
{
	if (strcasecmp(buf, "mbox") == 0)
		*dest = A_MBOX;
	else if (strcasecmp(buf, "maildir") == 0)
		*dest = A_MAILDIR;
	else if (strcasecmp(buf, "filename") == 0)
		*dest = A_FILENAME;
	else if (strcasecmp(buf, "mda") == 0)
		*dest = A_MDA;
	else
		return 0;
	return 1;
}

static int
ascii_load_mailaddr(struct mailaddr *dest, char *buf)
{
	if (! text_to_mailaddr(dest, buf))
		return 0;
	return 1;
}

static int
ascii_load_flags(enum envelope_flags *dest, char *buf)
{
	char *flag;

	while ((flag = strsep(&buf, " ,|")) != NULL) {
		if (strcasecmp(flag, "authenticated") == 0)
			*dest |= EF_AUTHENTICATED;
		else if (strcasecmp(flag, "enqueued") == 0)
			;
		else if (strcasecmp(flag, "bounce") == 0)
			*dest |= EF_BOUNCE;
		else if (strcasecmp(flag, "internal") == 0)
			*dest |= EF_INTERNAL;
		else
			return 0;
	}
	return 1;
}

static int
ascii_load_mta_relay_url(struct relayhost *relay, char *buf)
{
	if (! text_to_relayhost(relay, buf))
		return 0;
	return 1;
}

static int
ascii_load_bounce_type(enum bounce_type *dest, char *buf)
{
	if (strcasecmp(buf, "error") == 0)
		*dest = B_ERROR;
	else if (strcasecmp(buf, "warn") == 0)
		*dest = B_WARNING;
	else
		return 0;
	return 1;
}

static int
ascii_dump_uint16(uint16_t src, char *dest, size_t len)
{
	return bsnprintf(dest, len, "%d", src);
}

static int
ascii_dump_uint32(uint32_t src, char *dest, size_t len)
{
	return bsnprintf(dest, len, "%d", src);
}

static int
ascii_dump_time(time_t src, char *dest, size_t len)
{
	return bsnprintf(dest, len, "%" PRId64, (int64_t) src);
}

static int
ascii_dump_string(const char *src, char *dest, size_t len)
{
	return bsnprintf(dest, len, "%s", src);
}

static int
ascii_dump_type(enum delivery_type type, char *dest, size_t len)
{
	char *p = NULL;

	switch (type) {
	case D_MDA:
		p = "mda";
		break;
	case D_MTA:
		p = "mta";
		break;
	case D_BOUNCE:
		p = "bounce";
		break;
	default:
		return 0;
	}

	return bsnprintf(dest, len, "%s", p);
}

static int
ascii_dump_mda_method(enum action_type type, char *dest, size_t len)
{
	char *p = NULL;

	switch (type) {
	case A_MAILDIR:
		p = "maildir";
		break;
	case A_MBOX:
		p = "mbox";
		break;
	case A_FILENAME:
		p = "filename";
		break;
	case A_MDA:
		p = "mda";
		break;
	default:
		return 0;
	}
	return bsnprintf(dest, len, "%s", p);
}

static int
ascii_dump_mailaddr(struct mailaddr *addr, char *dest, size_t len)
{
	return bsnprintf(dest, len, "%s@%s",
	    addr->user, addr->domain);
}

static int
ascii_dump_flags(enum envelope_flags flags, char *buf, size_t len)
{
	size_t cpylen = 0;

	buf[0] = '\0';
	if (flags) {
		if (flags & EF_AUTHENTICATED)
			cpylen = strlcat(buf, "authenticated", len);
		if (flags & EF_BOUNCE) {
			if (buf[0] != '\0')
				strlcat(buf, " ", len);
			cpylen = strlcat(buf, "bounce", len);
		}
		if (flags & EF_INTERNAL) {
			if (buf[0] != '\0')
				strlcat(buf, " ", len);
			cpylen = strlcat(buf, "internal", len);
		}
	}

	return cpylen < len ? 1 : 0;
}

static int
ascii_dump_mta_relay_url(struct relayhost *relay, char *buf, size_t len)
{
	return bsnprintf(buf, len, "%s", relayhost_to_text(relay));
}

static int
ascii_dump_bounce_type(enum bounce_type type, char *dest, size_t len)
{
	char *p = NULL;

	switch (type) {
	case B_ERROR:
		p = "error";
		break;
	case B_WARNING:
		p = "warn";
		break;
	default:
		return 0;
	}
	return bsnprintf(dest, len, "%s", p);
}
