/*
 * Parsing INI-style configuration files. The routines are taken and
 * modified from SMB source code ( http://samba.anu.edu.au/cifs).
 */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "conf.h"

static const char *confname;
static char *bufr;
static int bsize;
static char *cursec;

/*
 * Scan to the end of a comment.
 */
static int eat_comment (FILE *fp)
{
	int c;

	c = getc (fp);
	while (c > 0 && c != '\n')
		c = getc (fp);
	return c;
}

static int eat_whitespace (FILE *fp)
{
	int c;

	c = getc (fp);
	while (isspace(c) && c != '\n')
		c = getc (fp);
	return c;
}

static int parse_continuation (char *line, int pos)
{
	pos--;
	while (pos >= 0 && isspace (line [pos]))
		pos--;
	if (pos < 0 || line[pos] != '\\') {
		line [pos+1] = 0;
		fprintf (stderr, "%s: invalid continuation line: '%s'\n",
			confname, line);
		exit (-1);
	}
	return pos;
}

/*
 * Scan a parameter name (or name and value pair) and pass the value (or
 * values) to function pfunc().
 */
static void parse_parameter (FILE *fp, void (*pfunc) (char*, char*, char*), int c)
{
	int i = 0;		/* position withing bufr */
	int end = 0;		/* bufr[end] is current end-of-string */
	int vstart = 0;		/* starting position of the parameter */

	/* Loop until we found the start of the value */
	while (vstart == 0) {
		/* Ensure there's space for next char */
		if (i > (bsize-2)) {
			bsize += 1024;
			bufr = realloc (bufr, bsize);
			if (! bufr) {
				fprintf (stderr, "%s: malloc failed\n", confname);
				exit (-1);
			}
		}
		switch (c) {
		case '=':
			if (end == 0) {
				fprintf (stderr, "%s: invalid parameter name\n", confname);
				exit (-1);
			}
			bufr[end++] = '\0';
			i = end;
			vstart = end;
			bufr[i] = '\0';
			break;

		case '\n':
			i = parse_continuation (bufr, i);
			end = ((i > 0) && (bufr[i-1] == ' ')) ? (i-1) : (i);
			c = getc (fp);
			break;

		case '\0':
		case EOF:
			bufr[i] = '\0';
			fprintf (stderr, "%s: unexpected end-of-file at %s: func\n",
				confname, bufr);
			exit (-1);

		default:
			if (isspace (c)) {
				bufr[end] = ' ';
				i = end + 1;
				c = eat_whitespace (fp);
			} else {
				bufr[i++] = c;
				end = i;
				c = getc (fp);
			}
			break;
		}
	}

	/* Now parse the value */
	c = eat_whitespace (fp);
	while (c > 0) {
		if (i > (bsize-2)) {
			bsize += 1024;
			bufr = realloc (bufr, bsize);
			if (! bufr) {
				fprintf (stderr, "%s: malloc failed\n", confname);
				exit (-1);
			}
		}
		switch(c) {
		case '\r':
			c = getc (fp);
			break;

		case '\n':
			i = parse_continuation (bufr, i);
			for (end=i; (end >= 0) && isspace (bufr[end]); end--)
				;
			c = getc (fp);
			break;

		default:
			bufr[i++] = c;
			if (! isspace (c))
				end = i;
			c = getc (fp);
			break;
		}
	}
	bufr[end] = '\0';
	pfunc (cursec, bufr, &bufr [vstart]);
}

/*
 * Scan a section name and remember it in `cursec'.
 */
static void parse_section (FILE *fp)
{
	int c, i, end;

	i = 0;
	end = 0;
	c = eat_whitespace (fp);	/* we've already got the '['. scan past initial white space */
	while (c > 0) {
		if (i > (bsize-2)) {
			bsize += 1024;
			bufr = realloc (bufr, bsize);
			if (! bufr) {
				fprintf (stderr, "%s: malloc failed\n", confname);
				exit (-1);
			}
		}
		switch (c) {
		case ']':		/* found the closing bracked */
			bufr[end] = '\0';
			if (end == 0) {
				fprintf (stderr, "%s: empty section name\n", confname);
				exit (-1);
			}

			/* Register a section. */
			if (cursec)
				free (cursec);
			cursec = strdup (bufr);

			eat_comment (fp);
			return;

		case '\n':
			i = parse_continuation (bufr, i);
			end = ((i > 0) && (bufr[i-1] == ' ')) ? (i-1) : (i);
			c = getc (fp);
			break;

		default:
			if (isspace (c)) {
				bufr[end] = ' ';
				i = end + 1;
				c = eat_whitespace (fp);
			} else {
				bufr[i++] = c;
				end = i;
				c = getc (fp);
			}
			break;
		}
	}
}

/*
 * Process the named parameter file
 */
void conf_parse (const char *filename, void (*pfunc) (char*, char*, char*))
{
	FILE *fp;
	int c;

	confname = filename;
	fp = fopen (filename, "r");
	if (! fp) {
		fprintf (stderr, "%s: unable to open config file\n", filename);
		exit (-1);
	}
	bsize = 1024;
	bufr = (char*) malloc (bsize);
	if (! bufr) {
		fprintf (stderr, "%s: malloc failed\n", confname);
		fclose (fp);
		exit (-1);
	}

	/* Parse file. */
	c = eat_whitespace (fp);
	while (c > 0) {
		switch (c) {
		case '\n':              	/* blank line */
			c = eat_whitespace (fp);
			break;
		case ';':               	/* comment line */
		case '#':
			c = eat_comment (fp);
			break;
		case '[':               	/* section header */
			parse_section (fp);
			c = eat_whitespace (fp);
			break;
		case '\\':              	/* bogus backslash */
			c = eat_whitespace (fp);
			break;
		default:                	/* parameter line */
			parse_parameter (fp, pfunc, c);
			c = eat_whitespace (fp);
			break;
		}
	}
	fclose (fp);
	if (cursec) {
		free (cursec);
		cursec = 0;
	}
	free (bufr);
	bufr = 0;
	bsize = 0;
}
