/*
 * Copyright (c) 2012, Oracle and/or its affiliates. All rights reserved.
 */

/***********************************************************************
 *                                                                      *
 *               This software is part of the ast package               *
 *          Copyright (c) 1992-2012 AT&T Intellectual Property          *
 *                      and is licensed under the                       *
 *                 Eclipse Public License, Version 1.0                  *
 *                    by AT&T Intellectual Property                     *
 *                                                                      *
 *                A copy of the License is available at                 *
 *          http://www.eclipse.org/org/documents/epl-v10.html           *
 *         (with md5 checksum b35adb5213ca9657e911e9befb180842)         *
 *                                                                      *
 *              Information and Software Systems Research               *
 *                            AT&T Research                             *
 *                           Florham Park NJ                            *
 *                                                                      *
 *               Glenn Fowler <glenn.s.fowler@gmail.com>                *
 *                    David Korn <dgkorn@gmail.com>                     *
 *                                                                      *
 ***********************************************************************/
/*
 * David Korn
 * Glenn Fowler
 * AT&T Bell Laboratories
 *
 * cmp
 */
#include "config_ast.h"  // IWYU pragma: keep

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "error.h"
#include "option.h"
#include "sfio.h"
#include "shcmd.h"

static const char usage[] =
    "[-?\n@(#)$Id: cmp (AT&T Research) 2010-04-11 $\n]" USAGE_LICENSE
    "[+NAME?cmp - compare two files]"
    "[+DESCRIPTION?\bcmp\b compares two files \afile1\a and \afile2\a. "
    "\bcmp\b writes no output if the files are the same. By default, if the "
    "files differ, the byte and line number at which the first difference "
    "occurred are written to standard output. Bytes and lines are numbered "
    "beginning with 1.]"
    "[+?If \askip1\a or \askip2\a are specified, or the \b-i\b option is "
    "specified, initial bytes of the corresponding file are skipped before "
    "beginning the compare. The skip values are in bytes or can have a "
    "suffix of \bk\b for kilobytes or \bm\b for megabytes.]"
    "[+?If either \afile1\a or \afiles2\a is \b-\b, \bcmp\b uses standard "
    "input starting at the current location.]"
    "[b:print-bytes?Print differing bytes as 3 digit octal values.]"
    "[c:print-chars?Print differing bytes as follows: non-space printable "
    "characters as themselves; space and control characters as \b^\b "
    "followed by a letter of the alphabet; and characters with the high bit "
    "set as the lower 7 bit character prefixed by \bM^\b for 7 bit space and "
    "non-printable characters and \bM-\b for all other characters. If the 7 "
    "bit character encoding is not ASCII then the characters are converted "
    "to ASCII to determine \ahigh bit set\a, and if set it is cleared and "
    "converted back to the native encoding. Multibyte characters in the "
    "current locale are treated as printable characters.]"
    "[d:differences?Print at most \adifferences\a differences using "
    "\b--verbose\b output format. \b--differences=0\b is equivalent to "
    "\b--silent\b.]#[differences]"
    "[i:ignore-initial|skip?Skip the the first \askip1\a bytes in \afile1\a "
    "and the first \askip2\a bytes in \afile2\a. If \askip2\a is omitted "
    "then \askip1\a is used.]:[skip1[::skip2]]:=0::0]"
    "[l:verbose?Write the decimal byte number and the differing bytes (in "
    "octal) for each difference.]"
    "[n:count|bytes?Compare at most \acount\a bytes.]#[count]"
    "[s:quiet|silent?Write nothing for differing files; return non-zero exit "
    "status only.]"
    "\n"
    "\nfile1 file2 [skip1 [skip2]]\n"
    "\n"
    "[+EXIT STATUS?]"
    "{"
    "[+0?The files or portions compared are identical.]"
    "[+1?The files are different.]"
    "[+>1?An error occurred.]"
    "}"
    "[+SEE ALSO?\bcomm\b(1), \bdiff\b(1), \bcat\b(1)]";

#define CMP_VERBOSE 0x01
#define CMP_SILENT 0x02
#define CMP_CHARS 0x04
#define CMP_BYTES 0x08

static void pretty(Sfio_t *out, int o, int delim, int flags) {
    int m;
    char *s;
    char buf[10];

    s = buf;
    if ((flags & CMP_BYTES) || !(flags & CMP_CHARS)) {
        *s++ = ' ';
        if ((flags & CMP_CHARS) && delim != -1) *s++ = ' ';
        *s++ = '0' + ((o >> 6) & 07);
        *s++ = '0' + ((o >> 3) & 07);
        *s++ = '0' + (o & 07);
    }
    if (flags & CMP_CHARS) {
        *s++ = ' ';
        if (o & 0x80) {
            m = 1;
            *s++ = 'M';
            o &= 0x7f;
        } else {
            m = 0;
        }
        if (isspace(o) || !isprint(o)) {
            if (!m) *s++ = ' ';
            *s++ = '^';
            o ^= 0x40;
        } else if (m) {
            *s++ = '-';
        } else {
            *s++ = ' ';
            *s++ = ' ';
        }
        *s++ = o;
    }
    *s = 0;
    sfputr(out, buf, delim);
}

/*
 * compare two files
 */

static int cmp(const char *file1, Sfio_t *f1, const char *file2, Sfio_t *f2, int flags,
               Sfoff_t count, Sfoff_t differences) {
    int c1;
    int c2;
    unsigned char *p1 = NULL;
    unsigned char *p2 = NULL;
    Sfoff_t lines = 1;
    unsigned char *e1 = NULL;
    unsigned char *e2 = NULL;
    Sfoff_t pos = 0;
    int n1 = 0;
    int ret = 0;
    unsigned char *last;

    for (;;) {
        if ((c1 = e1 - p1) <= 0) {
            if (count > 0 && !(count -= n1)) return ret;
            p1 = (unsigned char *)sfreserve(f1, SF_UNBOUND, 0);
            if (!p1 || (c1 = sfvalue(f1)) <= 0) {
                if (sferror(f1)) {
                    error(ERROR_exit(2), "read error on %s", file1);
                }
                if ((e2 - p2) > 0 || (sfreserve(f2, SF_UNBOUND, 0) && sfvalue(f2) > 0)) {
                    ret = 1;
                    if (!(flags & CMP_SILENT)) error(ERROR_exit(1), "EOF on %s", file1);
                }
                if (sferror(f2)) {
                    error(ERROR_exit(2), "read error on %s", file2);
                }
                return ret;
            }
            if (count > 0 && c1 > count) c1 = (int)count;
            e1 = p1 + c1;
            n1 = c1;
        }
        if ((c2 = e2 - p2) <= 0) {
            p2 = (unsigned char *)sfreserve(f2, SF_UNBOUND, 0);
            if (!p2 || (c2 = sfvalue(f2)) <= 0) {
                if (sferror(f2)) {
                    error(ERROR_exit(2), "read error on %s", file2);
                }
                if (!(flags & CMP_SILENT)) error(ERROR_exit(1), "EOF on %s", file2);
                return 1;
            }
            e2 = p2 + c2;
        }
        if (c1 > c2) c1 = c2;
        pos += c1;
        if (flags & CMP_SILENT) {
            if (memcmp(p1, p2, c1)) return 1;
            p1 += c1;
            p2 += c1;
        } else {
            last = p1 + c1;
            while (p1 < last) {
                if ((c1 = *p1++) != *p2++) {
                    if (differences >= 0) {
                        if (!differences) return 1;
                        differences--;
                    }
                    if (flags & CMP_VERBOSE) {
                        sfprintf(sfstdout, "%6I*d", sizeof(pos), pos - (last - p1));
                    } else {
                        sfprintf(sfstdout, "%s %s differ: char %I*d, line %I*u", file1, file2,
                                 sizeof(pos), pos - (last - p1), sizeof(lines), lines);
                    }
                    if (flags & (CMP_BYTES | CMP_CHARS | CMP_VERBOSE)) {
                        sfputc(sfstdout, (flags & CMP_VERBOSE) ? ' ' : ',');
                        pretty(sfstdout, c1, -1, flags);
                        pretty(sfstdout, *(p2 - 1), '\n', flags);
                    } else {
                        sfputc(sfstdout, '\n');
                    }
                    if (!differences || (differences < 0 && !(flags & CMP_VERBOSE))) return 1;
                    ret = 1;
                }
                if (c1 == '\n') lines++;
            }
        }
    }
}

int b_cmp(int argc, char **argv, Shbltin_t *context) {
    char *s;
    char *e;
    char *file1;
    char *file2;
    int n;
    struct stat s1;
    struct stat s2;

    Sfio_t *f1 = NULL;
    Sfio_t *f2 = NULL;
    Sfoff_t o1 = 0;
    Sfoff_t o2 = 0;
    Sfoff_t count = -1;
    Sfoff_t differences = -1;
    int flags = 0;

    if (cmdinit(argc, argv, context, 0)) return -1;
    while ((n = optget(argv, usage))) {
        switch (n) {  //!OCLINT(MissingDefaultStatement)
            case 'b':
                flags |= CMP_BYTES;
                break;
            case 'c':
                flags |= CMP_CHARS;
                break;
            case 'd':
                flags |= CMP_VERBOSE;
                differences = opt_info.number;
                break;
            case 'i':
                o1 = strtoll(opt_info.arg, &e, 0);
                if (*e == ':') {
                    o2 = strtoll(e + 1, &e, 0);
                } else {
                    o2 = o1;
                }
                if (*e) {
                    error(2, "%s: skip1:skip2 expected", opt_info.arg);
                    error(ERROR_usage(2), "%s", optusage(NULL));
                    __builtin_unreachable();
                }
                break;
            case 'l':
                flags |= CMP_VERBOSE;
                break;
            case 'n':
                count = opt_info.number;
                break;
            case 's':
                flags |= CMP_SILENT;
                break;
            case ':':
                error(2, "%s", opt_info.arg);
                break;
            case '?':
                error(ERROR_usage(2), "%s", opt_info.arg);
                __builtin_unreachable();
        }
    }
    argv += opt_info.index;
    if (error_info.errors || !(file1 = *argv++) || !(file2 = *argv++)) {
        error(ERROR_usage(2), "%s", optusage(NULL));
        __builtin_unreachable();
    }
    n = 2;

    if (!strcmp(file1, "-")) {
        f1 = sfstdin;
    } else if (!(f1 = sfopen(NULL, file1, "r"))) {
        if (!(flags & CMP_SILENT)) error(ERROR_system(0), "%s: cannot open", file1);
        goto done;
    }

    if (!strcmp(file2, "-")) {
        f2 = sfstdin;
    } else if (!(f2 = sfopen(NULL, file2, "r"))) {
        if (!(flags & CMP_SILENT)) error(ERROR_system(0), "%s: cannot open", file2);
        goto done;
    }
    s = *argv++;
    if (s) {
        o1 = strtoll(s, &e, 0);
        if (*e) {
            error(ERROR_exit(0), "%s: %s: invalid skip", file1, s);
            goto done;
        }
        s = *argv++;
        if (s) {
            o2 = strtoll(s, &e, 0);
            if (*e) {
                error(ERROR_exit(0), "%s: %s: invalid skip", file2, s);
                goto done;
            }
        }
        if (*argv) {
            error(ERROR_usage(0), "%s", optusage(NULL));
            goto done;
        }
    }
    if (o1 && sfseek(f1, o1, SEEK_SET) != o1) {
        if (!(flags & CMP_SILENT)) error(ERROR_exit(0), "EOF on %s", file1);
        n = 1;
        goto done;
    }
    if (o2 && sfseek(f2, o2, SEEK_SET) != o2) {
        if (!(flags & CMP_SILENT)) error(ERROR_exit(0), "EOF on %s", file2);
        n = 1;
        goto done;
    }
    if (fstat(sffileno(f1), &s1)) {
        error(ERROR_system(0), "%s: cannot stat", file1);
    } else if (fstat(sffileno(f2), &s2)) {
        error(ERROR_system(0), "%s: cannot stat", file1);
    } else if (s1.st_ino == s2.st_ino && s1.st_dev == s2.st_dev && o1 == o2) {
        n = 0;
    } else {
        n = ((flags & CMP_SILENT) && S_ISREG(s1.st_mode) && S_ISREG(s2.st_mode) &&
             (s1.st_size - o1) != (s2.st_size - o2))
                ? 1
                : cmp(file1, f1, file2, f2, flags, count, differences);
    }
done:
    if (f1 && f1 != sfstdin) sfclose(f1);
    if (f2 && f2 != sfstdin) sfclose(f2);
    return n;
}
