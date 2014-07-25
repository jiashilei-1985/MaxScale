/*
 * This file is distributed as part of MaxScale by SkySQL.  It is free
 * software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation,
 * version 2.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 51
 * Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Copyright SkySQL Ab 2014
 */
#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include <filter.h>
#include <modinfo.h>
#include <modutil.h>
#include <mysqlhint.h>

/**
 * hintparser.c - Find any comment in the SQL packet and look for MAXSCALE
 * hints in that comment.
 */

/**
 * The keywords in the hint syntax
 */
struct {
	char		*keyword;
	TOKEN_VALUE	token;
} keywords[] = {
	{ "maxscale",	TOK_MAXSCALE },
	{ "prepare",	TOK_PREPARE },
	{ "start",	TOK_START },
	{ "begin",	TOK_START },
	{ "stop",	TOK_STOP },
	{ "end",	TOK_STOP },
	{ "=",		TOK_EQUAL },
	{ "route",	TOK_ROUTE },
	{ "to",		TOK_TO },
	{ "master",	TOK_MASTER },
	{ "slave",	TOK_SLAVE },
	{ "server",	TOK_SERVER },
	{ NULL, 0 }
};

static HINT_TOKEN *hint_next_token(GWBUF **buf, char **ptr);
static void hint_pop(HINT_SESSION *);
static HINT *lookup_named_hint(HINT_SESSION *, char *);
static void create_named_hint(HINT_SESSION *, char *, HINT *);
static void hint_push(HINT_SESSION *, HINT *);

typedef enum { HM_EXECUTE, HM_START, HM_PREPARE } HINT_MODE;

/**
 * Parse the hint comments in the MySQL statement passed in request.
 * Add any hints to the buffer for later processing.
 *
 * @param session	The filter session
 * @param request	The MySQL request buffer
 * @return		The hints parsed in this statement or active on the
 *			stack
 */
HINT *
hint_parser(HINT_SESSION *session, GWBUF *request)
{
char		*ptr, lastch = ' ';
int		len, residual, state;
int		found, escape, quoted, squoted;
HINT		*rval = NULL;
char		*pname, *lvalue, *hintname = NULL;
GWBUF		*buf;
HINT_TOKEN	*tok;
HINT_MODE	mode = HM_EXECUTE;

	/* First look for any comment in the SQL */
	modutil_MySQL_Query(request, &ptr, &len, &residual);
	buf = request;
	found = 0;
	escape = 0;
	quoted = 0;
	squoted = 0;
	do {
		while (len--)
		{
			if (*ptr == '\\')
				escape = 1;
			else if (*ptr == '\"' && quoted)
				quoted = 0;
			else if (*ptr == '\"' && quoted == 0)
				quoted = 0;
			else if (*ptr == '\'' && squoted)
				squoted = 0;
			else if (*ptr == '\"' && squoted == 0)
				squoted = 0;
			else if (quoted || squoted)
				;
			else if (escape)
				escape = 0;
			else if (*ptr == '#')
			{
				found = 1;
				break;
			}
			else if (*ptr == '/')
				lastch = '/';
			else if (*ptr == '*' && lastch == '/')
			{
				found = 1;
				break;
			}
			else if (*ptr == '-' && lastch == '-')
			{
				found = 1;
				break;
			}
			else if (*ptr == '-')
				lastch = '-';
			else
				lastch = *ptr;
			ptr++;
		}
		if (found)
			break;

		buf = buf->next;
		if (buf)
		{
			len = GWBUF_LENGTH(buf);
			ptr = GWBUF_DATA(buf);
		}
	} while (buf);

	if (!found)		/* No comment so we need do no more */
	{
		goto retblock;
	}

	/*
	 * If we have got here then we have a comment, ptr point to
	 * the comment character if it is a '#' comment or the second
	 * character of the comment if it is a -- or /* comment
	 *
	 * Move to the next character in the SQL.
	 */
	ptr++;
	if (ptr > (char *)(buf->end))
	{
		buf = buf->next;
		if (buf)
			ptr = GWBUF_DATA(buf);
		else
			goto retblock;
	}

	tok = hint_next_token(&buf, &ptr);
	if (tok->token != TOK_MAXSCALE)
	{
		free(tok);
		goto retblock;
	}

	state = HS_INIT;
	while ((tok = hint_next_token(&buf, &ptr)) != NULL
					&& tok->token != TOK_EOL)
	{
		switch (state)
		{
		case HS_INIT:
			switch (tok->token)
			{
			case TOK_ROUTE:
				state = HS_ROUTE;
				break;
			case TOK_STRING:
				state = HS_NAME;
				lvalue = tok->value;
				break;
			case TOK_STOP:
				/* Action: pop active hint */
				hint_pop(session);
				state = HS_INIT;
				break;
			default:
				/* Error: expected hint, name or STOP */
				;
			}
			break;
		case HS_ROUTE:
			if (tok->token != TOK_TO)
				/* Error, expect TO */;
			state = HS_ROUTE1;
			break;
		case HS_ROUTE1:
			switch (tok->token)
			{
			case TOK_MASTER:
				rval = hint_create_route(rval,
					HINT_ROUTE_TO_MASTER, NULL);
				break;
			case TOK_SLAVE:
				rval = hint_create_route(rval,
					HINT_ROUTE_TO_SLAVE, NULL);
				break;
			case TOK_SERVER:
				state = HS_ROUTE_SERVER;
				break;
			default:
				/* Error expected MASTER, SLAVE or SERVER */
				;
			}
			break;
		case HS_ROUTE_SERVER:
			if (tok->token == TOK_STRING)
			{
				rval = hint_create_route(rval,
					HINT_ROUTE_TO_NAMED_SERVER, tok->value);
			}
			else
			{
				/* Error: Expected server name */
			}
		case HS_NAME:
			switch (tok->token)
			{
			case TOK_EQUAL:
				pname = lvalue;
				state = HS_PVALUE;
				break;
			case TOK_PREPARE:
				pname = lvalue;
				state = HS_PREPARE;
				break;
			case TOK_START:
				/* Action start(lvalue) */
				hintname = lvalue;
				mode = HM_START;
				state = HS_INIT;
				break;
			default:
				/* Error, token tok->value not expected */
				;
			}
			break;
		case HS_PVALUE:
			/* Action: pname = tok->value */
			rval = hint_create_parameter(rval, pname, tok->value);
			state = HS_INIT;
			break;
		case HS_PREPARE:
			mode = HM_PREPARE;
			hintname = lvalue;
			switch (tok->token)
			{
			case TOK_ROUTE:
				state = HS_ROUTE;
				break;
			case TOK_STRING:
				state = HS_NAME;
				lvalue = tok->value;
				break;
			default:
				/* Error, token tok->value not expected */
				;
			}
			break;
		}
		free(tok);
	}

	switch (mode)
	{
	case HM_START:
		/*
		 * We are starting either a predefined set of hints,
		 * creating a new set of hints and starting in a single
		 * operation or starting an annonymous block of hints.
		 */
		if (hintname == NULL && rval != NULL)
		{
			/* We are starting an anonymous block of hints */
			hint_push(session, rval);
			rval = NULL;
		} else if (hintname && rval)
		{
			/* We are creating and starting a block of hints */
			if (lookup_named_hint(session, hintname) != NULL)
			{
				/* Error hint with this name already exists */
			}
			else
			{
				create_named_hint(session, hintname, rval);
				hint_push(session, hint_dup(rval));
			}
		} else if (hintname && rval == NULL)
		{
			/* We starting an already define set of named hints */
			rval = lookup_named_hint(session, hintname);
			hint_push(session, hint_dup(rval));
			rval = NULL;
		} else if (hintname == NULL && rval == NULL)
		{
			/* Error case */
		}
		break;
	case HM_PREPARE:
		/*
		 * We are preparing a named set of hints. Note this does
		 * not trigger the usage of these hints currently.
		 */
		if (hintname == NULL || rval == NULL)
		{
			/* Error case, name and hints must be defined */
		}
		else
		{
			create_named_hint(session, hintname, rval);
		}
		/* We are not starting the hints now, so return an empty
		 * hint set.
		 */
		rval = NULL;
		break;
	case HM_EXECUTE:
		/*
		 * We have a one-off hint for the statement we are
		 * currently forwarding.
		 */
		break;
	}

retblock:
	if (rval == NULL)
	{
		/* No new hint parsed in this statement, apply the current
		 * top of stack if there is one.
		 */
		if (session->stack)
			rval = hint_dup(session->stack->hint);
	}
	return rval;
}

/**
 * Return the next token in the inout stream
 *
 * @param buf	A pointer to the buffer point, will be updated if a
 *		new buffer is used.
 * @param ptr	The pointer within the buffer we are processing
 * @return A HINT token
 */
static HINT_TOKEN *
hint_next_token(GWBUF **buf, char **ptr)
{
char		word[100], *dest;
int		inword = 0;
char		inquote = '\0';
int		i, found;
HINT_TOKEN	*tok;

	if ((tok = (HINT_TOKEN *)malloc(sizeof(HINT_TOKEN))) == NULL)
		return NULL;
	tok->value = NULL;
	dest = word;
	while (*ptr <= (char *)((*buf)->end) || (*buf)->next)
	{
		if (inword && inquote == '\0' &&
				(**ptr == '=' || isspace(**ptr)))
		{
			inword = 0;
			break;
		}
		else if (**ptr == '\'' && inquote == '\'')
			inquote = '\0';
		else if (**ptr == '\'')
			inquote = **ptr;
		else if (inword || (isspace(**ptr) == 0))
		{
			*dest++ = **ptr;
			inword = 1;
		}
		(*ptr)++;
		if (*ptr > (char *)((*buf)->end) && (*buf)->next)
		{
			*buf = (*buf)->next;
			*ptr = (*buf)->start;
		}
		if (dest - word > 98)
			break;
		
	}
	*dest = 0;

	/* We now have a word in the local word, check to see if it is a
	 * token we recognise.
	 */
	if (word[0] == '\0')
	{
		tok->token = TOK_EOL;
		return tok;
	}
	found = 0;
	for (i = 0; keywords[i].keyword; i++)
	{
		if (strcasecmp(word, keywords[i].keyword) == 0)
		{
			tok->token = keywords[i].token;
			found = 1;
			break;
		}
	}
	if (found == 0)
	{
		tok->token = TOK_STRING;
		tok->value = strdup(word);
	}

	return tok;
}


/**
 * hint_pop - pop the hint off the top of the stack if it is not empty
 *
 * @param	session	The filter session.
 */
void
hint_pop(HINT_SESSION *session)
{
HINTSTACK	*ptr;
HINT		*hint;

	if ((ptr = session->stack) != NULL)
	{
		session->stack = ptr->next;
		while (ptr->hint)
		{
			hint = ptr->hint;
			ptr->hint = hint->next;
			hint_free(hint);
		}
		free(ptr);
	}
}

/**
 * Push a hint onto the stack of actie hints
 *
 * @param session	The filter session
 * @param hint		The hint to push, the hint ownership is retained
 *			by the stack and should not be freed by the caller
 */
static void
hint_push(HINT_SESSION *session, HINT *hint)
{
HINTSTACK	*item;

	if ((item = (HINTSTACK *)malloc(sizeof(HINTSTACK))) == NULL)
		return;
	item->hint = hint;
	item->next = session->stack;
	session->stack = item;
}

/**
 * Search for a hint block that already exists with this name
 *
 * @param session	The filter session
 * @param name		The name to lookup
 * @return the HINT or NULL if the name was not found.
 */
static HINT *
lookup_named_hint(HINT_SESSION *session, char *name)
{
NAMEDHINTS	*ptr = session->named_hints;

	while (ptr)
	{
		if (strcmp(ptr->name, name) == 0)
			return ptr->hints;
		ptr = ptr->next;
	}
	return NULL;
}

/**
 * Create a named hint block
 *
 * @param session	The filter session
 * @param name		The name of the block to ceate
 * @param hint		The hints themselves
 */
static void
create_named_hint(HINT_SESSION *session, char *name, HINT *hint)
{
NAMEDHINTS	*block;

	if ((block = (NAMEDHINTS *)malloc(sizeof(NAMEDHINTS))) == NULL)
		return;
	if ((block->name = strdup(name)) == NULL)
	{
		free(block);
		return;
	}
	block->hints = hint;
	block->next = session->named_hints;
	session->named_hints = block;
}
