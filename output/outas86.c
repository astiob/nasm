/* ----------------------------------------------------------------------- *
 *   
 *   Copyright 1996-2009 The NASM Authors - All Rights Reserved
 *   See the file AUTHORS included with the NASM distribution for
 *   the specific copyright holders.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following
 *   conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *     
 *     THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND
 *     CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
 *     INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 *     MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 *     DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 *     CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *     SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 *     NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *     LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 *     HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 *     CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 *     OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
 *     EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * ----------------------------------------------------------------------- */

/* 
 * outas86.c	output routines for the Netwide Assembler to produce
 *		Linux as86 (bin86-0.3) object files
 */

#include "compiler.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <inttypes.h>

#include "nasm.h"
#include "nasmlib.h"
#include "saa.h"
#include "raa.h"
#include "output/outform.h"
#include "output/outlib.h"

#ifdef OF_AS86

struct Piece {
    struct Piece *next;
    int type;                   /* 0 = absolute, 1 = seg, 2 = sym */
    int32_t offset;		/* relative offset */
    int number;			/* symbol/segment number (4=bss) */
    int32_t bytes;	        /* size of reloc or of absolute data */
    bool relative;		/* relative address? */
};

struct Symbol {
    int32_t strpos;		/* string table position of name */
    int flags;                  /* symbol flags */
    int segment;                /* 4=bss at this point */
    int32_t value;		/* address, or COMMON variable size */
};

/*
 * Section IDs - used in Piece.number and Symbol.segment.
 */
#define SECT_TEXT 0             /* text section */
#define SECT_DATA 3             /* data section */
#define SECT_BSS 4              /* bss section */

/*
 * Flags used in Symbol.flags.
 */
#define SYM_ENTRY (1<<8)
#define SYM_EXPORT (1<<7)
#define SYM_IMPORT (1<<6)
#define SYM_ABSOLUTE (1<<4)

struct Section {
    struct SAA *data;
    uint32_t datalen, size, len;
    int32_t index;
    struct Piece *head, *last, **tail;
};

static char as86_module[FILENAME_MAX];

static struct Section stext, sdata;
static uint32_t bsslen;
static int32_t bssindex;

static struct SAA *syms;
static uint32_t nsyms;

static struct RAA *bsym;

static struct SAA *strs;
static uint32_t strslen;

static int as86_reloc_size;

static FILE *as86fp;
static efunc error;

static void as86_write(void);
static void as86_write_section(struct Section *, int);
static int as86_add_string(char *name);
static void as86_sect_write(struct Section *, const uint8_t *,
                            uint32_t);

static void as86_init(FILE * fp, efunc errfunc, ldfunc ldef, evalfunc eval)
{
    as86fp = fp;
    error = errfunc;
    (void)ldef;                 /* placate optimisers */
    (void)eval;
    stext.data = saa_init(1L);
    stext.datalen = 0L;
    stext.head = stext.last = NULL;
    stext.tail = &stext.head;
    sdata.data = saa_init(1L);
    sdata.datalen = 0L;
    sdata.head = sdata.last = NULL;
    sdata.tail = &sdata.head;
    bsslen =
        stext.len = stext.datalen = stext.size =
        sdata.len = sdata.datalen = sdata.size = 0;
    stext.index = seg_alloc();
    sdata.index = seg_alloc();
    bssindex = seg_alloc();
    syms = saa_init((int32_t)sizeof(struct Symbol));
    nsyms = 0;
    bsym = raa_init();
    strs = saa_init(1L);
    strslen = 0;

    as86_add_string(as86_module);
}

static void as86_cleanup(int debuginfo)
{
    struct Piece *p;

    (void)debuginfo;

    as86_write();
    fclose(as86fp);
    saa_free(stext.data);
    while (stext.head) {
        p = stext.head;
        stext.head = stext.head->next;
        nasm_free(p);
    }
    saa_free(sdata.data);
    while (sdata.head) {
        p = sdata.head;
        sdata.head = sdata.head->next;
        nasm_free(p);
    }
    saa_free(syms);
    raa_free(bsym);
    saa_free(strs);
}

static int32_t as86_section_names(char *name, int pass, int *bits)
{

    (void)pass;

    /*
     * Default is 16 bits.
     */
    if (!name)
        *bits = 16;

    if (!name)
        return stext.index;

    if (!strcmp(name, ".text"))
        return stext.index;
    else if (!strcmp(name, ".data"))
        return sdata.index;
    else if (!strcmp(name, ".bss"))
        return bssindex;
    else
        return NO_SEG;
}

static int as86_add_string(char *name)
{
    int pos = strslen;
    int length = strlen(name);

    saa_wbytes(strs, name, (int32_t)(length + 1));
    strslen += 1 + length;

    return pos;
}

static void as86_deflabel(char *name, int32_t segment, int64_t offset,
                          int is_global, char *special)
{
    bool is_start = false;
    struct Symbol *sym;

    if (special)
        error(ERR_NONFATAL, "as86 format does not support any"
              " special symbol types");


    if (name[0] == '.' && name[1] == '.' && name[2] != '@') {
	if (strcmp(name, "..start")) {
	    error(ERR_NONFATAL, "unrecognised special symbol `%s'", name);
	    return;
	} else {
	    is_start = true;
	}
    }

    sym = saa_wstruct(syms);

    sym->strpos = as86_add_string(name);
    sym->flags = 0;

    if (is_start)
      sym->flags = SYM_ENTRY;

    if (segment == NO_SEG)
        sym->flags |= SYM_ABSOLUTE, sym->segment = 0;
    else if (segment == stext.index)
        sym->segment = SECT_TEXT;
    else if (segment == sdata.index)
        sym->segment = SECT_DATA;
    else if (segment == bssindex)
        sym->segment = SECT_BSS;
    else {
        sym->flags |= SYM_IMPORT;
        sym->segment = 15;
    }

    if (is_global == 2)
        sym->segment = 3;       /* already have IMPORT */

    if (is_global && !(sym->flags & SYM_IMPORT))
        sym->flags |= SYM_EXPORT;

    sym->value = offset;

    /*
     * define the references from external-symbol segment numbers
     * to these symbol records.
     */
    if (segment != NO_SEG && segment != stext.index &&
        segment != sdata.index && segment != bssindex)
        bsym = raa_write(bsym, segment, nsyms);

    nsyms++;
}

static void as86_add_piece(struct Section *sect, int type, int32_t offset,
                           int32_t segment, int32_t bytes, int relative)
{
    struct Piece *p;

    sect->len += bytes;

    if (type == 0 && sect->last && sect->last->type == 0) {
        sect->last->bytes += bytes;
        return;
    }

    p = sect->last = *sect->tail = nasm_malloc(sizeof(struct Piece));
    sect->tail = &p->next;
    p->next = NULL;

    p->type = type;
    p->offset = offset;
    p->bytes = bytes;
    p->relative = relative;

    if (type == 1 && segment == stext.index)
        p->number = SECT_TEXT;
    else if (type == 1 && segment == sdata.index)
        p->number = SECT_DATA;
    else if (type == 1 && segment == bssindex)
        p->number = SECT_BSS;
    else if (type == 1)
        p->number = raa_read(bsym, segment), p->type = 2;
}

static void as86_out(int32_t segto, const void *data,
		     enum out_type type, uint64_t size,
                     int32_t segment, int32_t wrt)
{
    struct Section *s;
    int32_t offset;
    uint8_t mydata[4], *p;

    if (wrt != NO_SEG) {
        wrt = NO_SEG;           /* continue to do _something_ */
        error(ERR_NONFATAL, "WRT not supported by as86 output format");
    }

    /*
     * handle absolute-assembly (structure definitions)
     */
    if (segto == NO_SEG) {
        if (type != OUT_RESERVE)
            error(ERR_NONFATAL, "attempt to assemble code in [ABSOLUTE]"
                  " space");
        return;
    }

    if (segto == stext.index)
        s = &stext;
    else if (segto == sdata.index)
        s = &sdata;
    else if (segto == bssindex)
        s = NULL;
    else {
        error(ERR_WARNING, "attempt to assemble code in"
              " segment %d: defaulting to `.text'", segto);
        s = &stext;
    }

    if (!s && type != OUT_RESERVE) {
        error(ERR_WARNING, "attempt to initialize memory in the"
              " BSS section: ignored");
	bsslen += realsize(type, size);
        return;
    }

    if (type == OUT_RESERVE) {
        if (s) {
            error(ERR_WARNING, "uninitialized space declared in"
                  " %s section: zeroing",
                  (segto == stext.index ? "code" : "data"));
            as86_sect_write(s, NULL, size);
            as86_add_piece(s, 0, 0L, 0L, size, 0);
        } else
            bsslen += size;
    } else if (type == OUT_RAWDATA) {
        if (segment != NO_SEG)
            error(ERR_PANIC, "OUT_RAWDATA with other than NO_SEG");
        as86_sect_write(s, data, size);
        as86_add_piece(s, 0, 0L, 0L, size, 0);
    } else if (type == OUT_ADDRESS) {
        if (segment != NO_SEG) {
            if (segment % 2) {
                error(ERR_NONFATAL, "as86 format does not support"
                      " segment base references");
            } else {
                offset = *(int64_t *)data;
                as86_add_piece(s, 1, offset, segment, size, 0);
            }
        } else {
            p = mydata;
            WRITELONG(p, *(int64_t *)data);
            as86_sect_write(s, data, size);
            as86_add_piece(s, 0, 0L, 0L, size, 0);
        }
    } else if (type == OUT_REL2ADR) {
        if (segment == segto)
            error(ERR_PANIC, "intra-segment OUT_REL2ADR");
        if (segment != NO_SEG) {
            if (segment % 2) {
                error(ERR_NONFATAL, "as86 format does not support"
                      " segment base references");
            } else {
                offset = *(int64_t *)data;
                as86_add_piece(s, 1, offset - size + 2, segment, 2L,
                               1);
            }
        }
    } else if (type == OUT_REL4ADR) {
        if (segment == segto)
            error(ERR_PANIC, "intra-segment OUT_REL4ADR");
        if (segment != NO_SEG) {
            if (segment % 2) {
                error(ERR_NONFATAL, "as86 format does not support"
                      " segment base references");
            } else {
                offset = *(int64_t *)data;
                as86_add_piece(s, 1, offset - size + 4, segment, 4L,
                               1);
            }
        }
    }
}

static void as86_write(void)
{
    uint32_t i;
    int32_t symlen, seglen, segsize;

    /*
     * First, go through the symbol records working out how big
     * each will be. Also fix up BSS references at this time, and
     * set the flags words up completely.
     */
    symlen = 0;
    saa_rewind(syms);
    for (i = 0; i < nsyms; i++) {
        struct Symbol *sym = saa_rstruct(syms);
        if (sym->segment == SECT_BSS)
            sym->segment = SECT_DATA, sym->value += sdata.len;
        sym->flags |= sym->segment;
        if (sym->value == 0)
            sym->flags |= 0 << 14, symlen += 4;
        else if (sym->value >= 0 && sym->value <= 255)
            sym->flags |= 1 << 14, symlen += 5;
        else if (sym->value >= 0 && sym->value <= 65535L)
            sym->flags |= 2 << 14, symlen += 6;
        else
            sym->flags |= 3 << 14, symlen += 8;
    }

    /*
     * Now do the same for the segments, and get the segment size
     * descriptor word at the same time.
     */
    seglen = segsize = 0;
    if ((uint32_t)stext.len > 65535L)
        segsize |= 0x03000000L, seglen += 4;
    else
        segsize |= 0x02000000L, seglen += 2;
    if ((uint32_t)sdata.len > 65535L)
        segsize |= 0xC0000000L, seglen += 4;
    else
        segsize |= 0x80000000L, seglen += 2;

    /*
     * Emit the as86 header.
     */
    fwriteint32_t(0x000186A3L, as86fp);
    fputc(0x2A, as86fp);
    fwriteint32_t(27 + symlen + seglen + strslen, as86fp); /* header length */
    fwriteint32_t(stext.len + sdata.len + bsslen, as86fp);
    fwriteint16_t(strslen, as86fp);
    fwriteint16_t(0, as86fp);     /* class = revision = 0 */
    fwriteint32_t(0x55555555L, as86fp);    /* segment max sizes: always this */
    fwriteint32_t(segsize, as86fp);        /* segment size descriptors */
    if (segsize & 0x01000000L)
        fwriteint32_t(stext.len, as86fp);
    else
        fwriteint16_t(stext.len, as86fp);
    if (segsize & 0x40000000L)
        fwriteint32_t(sdata.len + bsslen, as86fp);
    else
        fwriteint16_t(sdata.len + bsslen, as86fp);
    fwriteint16_t(nsyms, as86fp);

    /*
     * Write the symbol table.
     */
    saa_rewind(syms);
    for (i = 0; i < nsyms; i++) {
        struct Symbol *sym = saa_rstruct(syms);
        fwriteint16_t(sym->strpos, as86fp);
        fwriteint16_t(sym->flags, as86fp);
        switch (sym->flags & (3 << 14)) {
        case 0 << 14:
            break;
        case 1 << 14:
            fputc(sym->value, as86fp);
            break;
        case 2 << 14:
            fwriteint16_t(sym->value, as86fp);
            break;
        case 3 << 14:
            fwriteint32_t(sym->value, as86fp);
            break;
        }
    }

    /*
     * Write out the string table.
     */
    saa_fpwrite(strs, as86fp);

    /*
     * Write the program text.
     */
    as86_reloc_size = -1;
    as86_write_section(&stext, SECT_TEXT);
    as86_write_section(&sdata, SECT_DATA);
    /*
     * Append the BSS section to the .data section
     */
    if (bsslen > 65535L) {
        fputc(0x13, as86fp);
        fwriteint32_t(bsslen, as86fp);
    } else if (bsslen > 255) {
        fputc(0x12, as86fp);
        fwriteint16_t(bsslen, as86fp);
    } else if (bsslen) {
        fputc(0x11, as86fp);
        fputc(bsslen, as86fp);
    }

    fputc(0, as86fp);           /* termination */
}

static void as86_set_rsize(int size)
{
    if (as86_reloc_size != size) {
        switch (as86_reloc_size = size) {
        case 1:
            fputc(0x01, as86fp);
            break;
        case 2:
            fputc(0x02, as86fp);
            break;
        case 4:
            fputc(0x03, as86fp);
            break;
        default:
            error(ERR_PANIC, "bizarre relocation size %d", size);
        }
    }
}

static void as86_write_section(struct Section *sect, int index)
{
    struct Piece *p;
    uint32_t s;
    int32_t length;

    fputc(0x20 + index, as86fp);        /* select the right section */

    saa_rewind(sect->data);

    for (p = sect->head; p; p = p->next)
        switch (p->type) {
        case 0:
            /*
             * Absolute data. Emit it in chunks of at most 64
             * bytes.
             */
            length = p->bytes;
            do {
                char buf[64];
                int32_t tmplen = (length > 64 ? 64 : length);
                fputc(0x40 | (tmplen & 0x3F), as86fp);
                saa_rnbytes(sect->data, buf, tmplen);
                fwrite(buf, 1, tmplen, as86fp);
                length -= tmplen;
            } while (length > 0);
            break;
        case 1:
            /*
             * A segment-type relocation. First fix up the BSS.
             */
            if (p->number == SECT_BSS)
                p->number = SECT_DATA, p->offset += sdata.len;
            as86_set_rsize(p->bytes);
            fputc(0x80 | (p->relative ? 0x20 : 0) | p->number, as86fp);
            if (as86_reloc_size == 2)
                fwriteint16_t(p->offset, as86fp);
            else
                fwriteint32_t(p->offset, as86fp);
            break;
        case 2:
            /*
             * A symbol-type relocation.
             */
            as86_set_rsize(p->bytes);
            s = p->offset;
            if (s > 65535L)
                s = 3;
            else if (s > 255)
                s = 2;
            else if (s > 0)
                s = 1;
            else
                s = 0;
            fputc(0xC0 |
                  (p->relative ? 0x20 : 0) |
                  (p->number > 255 ? 0x04 : 0) | s, as86fp);
            if (p->number > 255)
                fwriteint16_t(p->number, as86fp);
            else
                fputc(p->number, as86fp);
            switch ((int)s) {
            case 0:
                break;
            case 1:
                fputc(p->offset, as86fp);
                break;
            case 2:
                fwriteint16_t(p->offset, as86fp);
                break;
            case 3:
                fwriteint32_t(p->offset, as86fp);
                break;
            }
            break;
        }
}

static void as86_sect_write(struct Section *sect,
                            const uint8_t *data, uint32_t len)
{
    saa_wbytes(sect->data, data, len);
    sect->datalen += len;
}

static int32_t as86_segbase(int32_t segment)
{
    return segment;
}

static int as86_directive(char *directive, char *value, int pass)
{
    (void)directive;
    (void)value;
    (void)pass;
    return 0;
}

static void as86_filename(char *inname, char *outname, efunc error)
{
    char *p;

    if ((p = strrchr(inname, '.')) != NULL) {
        strncpy(as86_module, inname, p - inname);
        as86_module[p - inname] = '\0';
    } else
        strcpy(as86_module, inname);

    standard_extension(inname, outname, ".o", error);
}

extern macros_t as86_stdmac[];

static int as86_set_info(enum geninfo type, char **val)
{
    (void)type;
    (void)val;
    return 0;
}
void as86_linenumber(char *name, int32_t segment, int32_t offset, int is_main,
                     int lineno)
{
    (void)name;
    (void)segment;
    (void)offset;
    (void)is_main;
    (void)lineno;
}
struct ofmt of_as86 = {
    "Linux as86 (bin86 version 0.3) object files",
    "as86",
    0,
    null_debug_arr,
    &null_debug_form,
    as86_stdmac,
    as86_init,
    as86_set_info,
    as86_out,
    as86_deflabel,
    as86_section_names,
    as86_segbase,
    as86_directive,
    as86_filename,
    as86_cleanup
};

#endif                          /* OF_AS86 */
