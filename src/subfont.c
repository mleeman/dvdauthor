/*
    Lowest-level interface to FreeType font rendering
*/
/* Copyright (C) 2000 - 2003 various authors of the MPLAYER project
 * This module uses various parts of the MPLAYER project (http://www.mplayerhq.hu)
 * With many changes by Sjef van Gool (svangool@hotmail.com) November 2003
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307
 * USA
 */

/*
 * Renders antialiased fonts for mplayer using freetype library.
 * Should work with TrueType, Type1 and any other font supported by libfreetype.
 *
 * Artur Zaprzala <zybi@fanthom.irc.pl>
 *
 * ported inside mplayer by Jindrich Makovicka
 * <makovick@kmlinux.fjfi.cvut.cz>
 *
 */

#include "config.h"


#ifdef HAVE_FREETYPE

#include "compat.h"

#include <math.h>

#ifdef HAVE_ICONV
#include <iconv.h>
#endif

#include <netinet/in.h>

#include "subconfig.h"
#include "subglobals.h"
#include "subfont.h"

#include FT_GLYPH_H

#if (FREETYPE_MAJOR > 2) || (FREETYPE_MAJOR == 2 && FREETYPE_MINOR >= 1)
#define HAVE_FREETYPE21
#endif

static int vo_image_width = 0;
static int vo_image_height = 0;
static int using_freetype = 0;
static char *subtitle_font_encoding = NULL;

//// constants
static unsigned int const colors = 256;
static unsigned int const maxcolor = 255;
static unsigned const base = 256;
static unsigned const first_char = 33; /* first non-printable, non-whitespace character */
#define MAX_CHARSET_SIZE 60000 /* hope this is enough! */

static FT_Library library;

#define f266ToInt(x)        (((x)+32)>>6)   // round fractional fixed point number to integer
                        // coordinates are in 26.6 pixels (i.e. 1/64th of pixels)
#define f266CeilToInt(x)    (((x)+63)>>6)   // ceiling
#define f266FloorToInt(x)   ((x)>>6)    // floor
#define f1616ToInt(x)       (((x)+0x8000)>>16)  // 16.16
#define floatTof266(x)      ((int)((x)*(1<<6)+0.5))

#define ALIGN_8BYTES(x)                (((x)+7)&~7)    // 8 byte align

#define WARNING(msg, args...)      fprintf(stderr,"WARN:" msg "\n", ## args)

#define DEBUG 0

//static double ttime;

static char *get_config_path(const char *filename)
  /* returns the path at which to find the config file named filename. If this is
    null, then just the config directory is returned. */
  {
    char *homedir;
    char *buff;
#if defined(__MINGW32__) || defined(__CYGWIN__)
    char exedir[260];
    static char *config_dir = "/spumux";
#else
    static char *config_dir = "/.spumux";
#endif
    int len;
#if defined(__MINGW32__) || defined(__CYGWIN__)
    if ((homedir = getenv("WINDIR")) != NULL)
        config_dir = "/fonts";
    else
#endif
    if ((homedir = getenv("HOME")) == NULL)
#if defined(__MINGW32__) || defined(__CYGWIN__) /*hack to get fonts etc. loaded outside of cygwin environment*/
      {
        extern int __stdcall GetModuleFileNameA(void* hModule, char* lpFilename, int nSize);
        int i, imax = 0;
        GetModuleFileNameA(NULL, exedir, sizeof exedir);
        for (i = 0; i<   strlen(exedir); i++)
            if (exedir[i] == '\\')
              {
                exedir[i] = '/'; /* translate path separator */
                imax = i;
              } /*if; for*/
        exedir[imax] = '\0'; /* terminate at rightmost path separator */
        homedir = exedir;
  /*    fprintf(stderr, "Homedir %s",homedir); */
      } /*if*/
#else
        return NULL;
#endif
    len = strlen(homedir) + strlen(config_dir) + 1;
    if (filename == NULL)
      {
        if ((buff = (char *)malloc(len)) == NULL)
            return NULL;
        sprintf(buff, "%s%s", homedir, config_dir);
      }
    else
      {
        len += strlen(filename) + 1;
        if ((buff = (char *)malloc(len)) == NULL)
            return NULL;
        sprintf(buff, "%s%s/%s", homedir, config_dir, filename);
      } /*if*/
/*  fprintf(stderr,"INFO: get_config_path('%s') -> '%s'\n",filename,buff); */
    return buff;
  } /*get_config_path*/

static void paste_bitmap
  (
    unsigned char *bbuffer,
    const FT_Bitmap *bitmap,
    int x, /* position from origin of bbuffer */
    int y, /* position from origin of bbuffer */
    int width, /* width of bbuffer */
    int height, /* height of area to copy */
    int bwidth /* width of area to copy */
  )
  /* copies pixels out of bitmap into bbuffer. */
  {
    int drow = x + y * width;
    int srow = 0;
    int sp, dp, w, h;
    if (bitmap->pixel_mode == FT_PIXEL_MODE_MONO)
      /* map one-bit-per-pixel source to 8 bits per pixel */
        for
          (
            h = bitmap->rows;
            h > 0 && height > 0;
            --h, height--, drow += width, srow += bitmap->pitch
          )
            for (w = bwidth, sp = dp = 0; w > 0; --w, ++dp, ++sp)
                bbuffer[drow + dp] =
                    (bitmap->buffer[srow + sp / 8] & 0x80 >> sp % 8) != 0 ?
                        255
                    :
                        0;
    else
      /* assume FT_PIXEL_MODE_GRAY, copy pixels as is */
        for
          (
            h = bitmap->rows;
            h > 0 && height > 0;
            --h, height--, drow += width, srow += bitmap->pitch
          )
            for (w = bwidth, sp = dp = 0; w > 0; --w, ++dp, ++sp)
                bbuffer[drow + dp] = bitmap->buffer[srow + sp];
  } /*paste_bitmap*/

static int check_font
  (
    font_desc_t *desc,
    float ppem,
    int padding,
    int pic_idx, /* which face to select from desc->faces */
    int charset_size, /* length of charset and charcodes arrays */
    const FT_ULong *charset, /* character codes to get glyphs for */
    const FT_ULong *charcodes,
      /* corresponding Unicode character codes, unless charset entries are already Unicode */
    int unicode /* whether charset entries are already Unicode */
  )
  /* computes various information about the specified font and puts it into desc. */
  {
    FT_Error error;
    const FT_Face face = desc->faces[pic_idx];
    int const load_flags = FT_LOAD_NO_HINTING | FT_LOAD_MONOCHROME | FT_LOAD_RENDER;
  /* int const load_flags = FT_LOAD_NO_HINTING; */ /* Anti-aliasing */
    raw_file * const pic_b = desc->pic_b[pic_idx];
    int ymin = INT_MAX, ymax = INT_MIN;
    int space_advance = 20;
    int width, height;
    int i, uni_charmap = 1;
    error = FT_Select_Charmap(face, FT_ENCODING_UNICODE);
//  fprintf(stderr, "select unicode charmap: %d\n", error);
    if (face->charmap == NULL || face->charmap->encoding != FT_ENCODING_UNICODE)
      {
        WARNING("Unicode charmap not available for this font. Very bad!");
        uni_charmap = 0; /* fallback to whatever encoding is available */
        error = FT_Set_Charmap(face, face->charmaps[0]);
        if (error)
            WARNING("No charmaps! Strange.");
      } /*if*/
  /* set size */
    if (FT_IS_SCALABLE(face))
      {
        error = FT_Set_Char_Size
          (
            /*face =*/ face,
            /*char_width =*/ 0, /* use height */
            /*char_height =*/ floatTof266(ppem),
            /*horiz_resolution =*/ 0, /* use 72dpi */
            /*vert_resolution =*/ 0 /* what he said */
          );
        if (error)
            WARNING("FT_Set_Char_Size failed.");
      }
    else
      {
        int j = 0;
        int jppem = face->available_sizes[0].height;
        /* find closest size */
        for (i = 0; i < face->num_fixed_sizes; ++i)
          {
            if
              (
                    fabs(face->available_sizes[i].height - ppem)
                <
                    abs(face->available_sizes[i].height - jppem)
              )
              {
                j = i;
                jppem = face->available_sizes[i].height;
              } /*if*/
          } /*for*/
        WARNING("Selected font is not scalable. Using ppem=%i.", face->available_sizes[j].height);
        error = FT_Set_Pixel_Sizes
          (
            /*face =*/ face,
            /*pixel_width =*/ face->available_sizes[j].width,
            /*pixel_height =*/ face->available_sizes[j].height
          );
        if (error)
            WARNING("FT_Set_Pixel_Sizes failed.");
      } /*if*/
    if (FT_IS_FIXED_WIDTH(face))
        WARNING("Selected font is fixed-width.");
  /* compute space advance */
    error = FT_Load_Char(face, ' ', load_flags);
    if (error)
        WARNING("spacewidth set to default.");
    else
        space_advance = f266ToInt(face->glyph->advance.x);
    if (!desc->spacewidth)
        desc->spacewidth = 2 * padding + space_advance;
    if (!desc->charspace)
        desc->charspace = -2 * padding;
    if (!desc->height)
        desc->height = f266ToInt(face->size->metrics.height);
    for (i = 0; i < charset_size; ++i)
      {
        const FT_ULong character = charset[i];
        const FT_ULong charunicode = charcodes[i];
        FT_UInt glyph_index;
        desc->font[unicode ? character : charunicode] = pic_idx;
        // get glyph index
        if (character == 0)
            glyph_index = 0;
        else
          {
            glyph_index = FT_Get_Char_Index
              (
                face,
                uni_charmap ? /* fixme: wrong, because charunicode is meaningless if unicode */
                    character
                :
                    charunicode
              );
            if (glyph_index == 0)
              {
                WARNING("Glyph for char 0x%02x|U+%04X|%c not found.",
                    (unsigned int)charunicode,
                    (unsigned int)character,
                    charunicode < ' ' || charunicode > 255 ? '.' : (unsigned int)charunicode);
                desc->font[unicode ? character : charunicode] = -1;
                continue;
              } /*if*/
          } /*if*/
        desc->glyph_index[unicode ? character : charunicode] = glyph_index;
      } /*for*/
//  fprintf(stderr, "font height: %lf\n", (double)(face->bbox.yMax - face->bbox.yMin) / (double)face->units_per_EM * ppem);
//  fprintf(stderr, "font width: %lf\n", (double)(face->bbox.xMax - face->bbox.xMin)/(double)face - >units_per_EM * ppem);
    ymax = (double)face->bbox.yMax / (double)face->units_per_EM * ppem + 1;
    ymin = (double)face->bbox.yMin / (double)face->units_per_EM * ppem - 1;
    width = ppem * (face->bbox.xMax - face->bbox.xMin) / face->units_per_EM + 3 + 2 * padding;
    if (desc->max_width < width)
        desc->max_width = width;
    width = ALIGN_8BYTES(width);
    pic_b->charwidth = width;
    if (width <= 0)
      {
        fprintf(stderr, "ERR: Wrong bounding box, width <= 0 !\n");
        return -1;
      } /*if*/
    if (ymax <= ymin)
      {
        fprintf(stderr, "ERR: Something went wrong. Use the source!\n");
        return -1;
      } /*if*/
    height = ymax - ymin + 2 * padding;
    if (height <= 0)
      {
        fprintf(stderr, "ERR: Wrong bounding box, height <= 0 !\n");
        return -1;
      } /*if*/
    if (desc->max_height < height)
        desc->max_height = height;
    pic_b->charheight = height;
//  fprintf(stderr, "font height2: %d\n", height);
    pic_b->baseline = ymax + padding;
    pic_b->padding = padding;
    pic_b->current_alloc = 0;
    pic_b->current_count = 0;
    pic_b->w = width;
    pic_b->h = height;
    pic_b->c = colors;
    pic_b->bmp = NULL;
    pic_b->pen = 0;
    return 0;
  } /*check_font*/

// general outline
static void outline
  (
    const unsigned char *s,
    unsigned char *t,
    int width,
    int height,
    int stride,
    const unsigned char *m,
    int r,
    int mwidth,
    int msize
  )
  {
    int x, y;
    for (y = 0; y < height; y++)
      {
        for (x = 0; x < width; x++)
          {
            const int src = s[x];
            if (src != 0)
              {
                const int x1 = x < r ? r - x : 0;
                const int y1 = y < r ? r - y : 0;
                const int x2 = x + r >= width ? r + width - x : 2 * r + 1;
                const int y2 = y + r >= height ? r + height - y : 2 * r + 1;
                register unsigned char * dstp = t + (y1 + y - r) * stride + x - r;
                //register int *mp = m + y1 * mwidth;
                const register unsigned char * mp = m + msize * src + y1 * mwidth;
                int my;
                for (my = y1; my < y2; my++)
                  {
                    register int mx;
                    for (mx = x1; mx < x2; mx++)
                      {
                        if(dstp[mx] < mp[mx])
                            dstp[mx] = mp[mx];
                      } /*for*/
                    dstp += stride;
                    mp += mwidth;
                  } /*for*/
              } /*if*/
          } /*for*/
        s += stride;
      } /*for*/
  } /*outline*/

// 1 pixel outline
static void outline1
  (
    const unsigned char *s,
    unsigned char *t,
    int width,
    int height,
    int stride
  )
  {
    const int skip = stride - width; /* for skipping rest of each row */
    int x, y;
    for (x = 0; x < width; ++x, ++s, ++t) /* copy first row as is */
        *t = *s;
    s += skip;
    t += skip;
    for (y = 1; y < height - 1; ++y)
      {
        *t++ = *s++; /* copy first column as is */
        for (x = 1; x < width - 1; ++x, ++s, ++t)
          {
            const unsigned int v =
                        (
                            s[-1 - stride] /* upper-left neighbour */
                        +
                            s[-1 + stride] /* lower-left neighbour */
                        +
                            s[+1 - stride] /* upper-right neighbour */
                        +
                            s[+1 + stride] /* lower-right neighbour */
                        )
                    /
                        2
                +
                    (
                        s[-1] /* left neighbour */
                    +
                        s[+1] /* right neighbour */
                    +
                        s[-stride] /* upper neighbour */
                    +
                        s[+stride] /* lower neighbour */
                    +
                        s[0] /* pixel itself */
                    );
            *t = v > maxcolor ? maxcolor : v;
          } /*for*/
        *t++ = *s++; /* copy last column as is */
        s += skip;
        t += skip;
      } /*for*/
    for (x = 0; x < width; ++x, ++s, ++t) /* copy last row as is */
        *t = *s;
  } /*outline1*/

// "0 pixel outline"
static void outline0
  (
    const unsigned char *s,
    unsigned char *t,
    int width,
    int height,
    int stride
  )
  {
    int y;
    for (y = 0; y < height; ++y) /* just copy all pixels as is */
      {
        memcpy(t, s, width);
        s += stride;
        t += stride;
      } /*for*/
  } /*outline0*/

// gaussian blur
static void blur
  (
    unsigned char *buffer,
    unsigned short *tmp2,
    int width,
    int height,
    int stride,
    const unsigned int *m2,
    int r,
    int mwidth
  )
  {
    int x, y;
    unsigned char * s = buffer;
    unsigned short *t = tmp2 + 1;
    for (y = 0; y < height; y++)
      {
        memset(t - 1, 0, (width + 1) * sizeof(short));
        for (x = 0; x < r; x++)
          {
            const int src = s[x];
            if (src)
              {
                register unsigned short * const dstp = t + x - r;
                int mx;
                const unsigned int * const m3 = m2 + src * mwidth;
                for (mx = r - x; mx < mwidth; mx++)
                  {
                    dstp[mx] += m3[mx];
                  } /*for*/
              } /*if*/
          } /*for*/
        for(; x < width - r; x++)
          {
            const int src = s[x];
            if (src)
              {
                register unsigned short * const dstp = t + x - r;
                int mx;
                const unsigned int * const m3 = m2 + src * mwidth;
                for (mx = 0; mx < mwidth; mx++)
                  {
                    dstp[mx] += m3[mx];
                  } /*for*/
              } /*if*/
          } /*for*/
        for(; x < width; x++)
          {
            const int src = s[x];
            if (src)
              {
                register unsigned short * const dstp = t + x - r;
                int mx;
                const int x2 = r + width - x;
                const unsigned int * const m3 = m2 + src * mwidth;
                for (mx = 0; mx < x2; mx++)
                  {
                    dstp[mx] += m3[mx];
                  } /*for*/
              } /*if*/
          } /*for*/
        s += stride;
        t += width + 1;
      } /*for*/
    t = tmp2;
    for (x = 0; x < width; x++)
      {
        for (y = 0; y < r; y++)
          {
            unsigned short * const srcp = t + y * (width + 1) + 1;
            const int src = *srcp;
            if (src)
              {
                register unsigned short * dstp = srcp - 1 + width + 1;
                const int src2 = src + 128 >> 8;
                const unsigned int * const m3 = m2 + src2 * mwidth;
                int mx;
                *srcp = 128;
                for (mx = r - 1; mx < mwidth; mx++)
                  {
                    *dstp += m3[mx];
                    dstp += width + 1;
                  } /*for*/
              } /*if*/
          } /*for*/
        for (; y < height - r; y++)
          {
            unsigned short * const srcp = t + y * (width + 1) + 1;
            const int src = *srcp;
            if (src)
              {
                register unsigned short * dstp = srcp - 1 - r * (width + 1);
                const int src2 = src + 128 >> 8;
                const unsigned int * const m3 = m2 + src2 * mwidth;
                int mx;
                *srcp = 128;
                for (mx = 0; mx < mwidth; mx++)
                  {
                    *dstp += m3[mx];
                    dstp += width + 1;
                  } /*for*/
              } /*if*/
          } /*for*/
        for (; y < height; y++)
          {
            unsigned short * const srcp = t + y * (width + 1) + 1;
            const int src = *srcp;
            if (src)
              {
                const int y2 = r + height - y;
                register unsigned short * dstp = srcp - 1 - r * (width + 1);
                const int src2 = src + 128 >> 8;
                const unsigned int * const m3 = m2 + src2 * mwidth;
                int mx;
                *srcp = 128;
                for (mx = 0; mx < y2; mx++)
                  {
                    *dstp += m3[mx];
                    dstp += width + 1;
                  } /*for*/
              } /*if*/
          } /*for*/
        t++;
      } /*for*/
    t = tmp2;
    s = buffer;
    for (y = 0; y < height; y++)
      {
        for (x = 0; x < width; x++)
          {
            s[x] = t[x] >> 8;
          } /*for*/
        s += stride;
        t += width + 1;
      } /*for*/
  } /*blur*/

static void resample_alpha
  (
    unsigned char *abuf,
    const unsigned char *bbuf,
    int width, /* dimensions of both buffers */
    int height,
    int stride, /* width in bytes of both buffers */
    float factor
  )
  /* copies 8-bit pixels from bbuf to abuf, amplitudes scaled by factor. */
  {
    int f = factor * 256.0f;
    int i, j;
    for (i = 0; i < height; i++)
      {
        unsigned char * a = abuf + i * stride;
        const unsigned char * b = bbuf + i * stride;
        for (j = 0; j < width; j++, a++, b++)
          {
            int x = *a;   // alpha
            const int y = *b;   // bitmap
            x = 255 - (x * f >> 8); // scale
            if (x + y > 255)
                x = 255 - y; // to avoid overflows
            if (x < 1)
                x = 1;
            else if (x >= 252)
                x = 0;
            *a = x;
          } /*for*/
      } /*for*/
  } /*resample_alpha*/

void render_one_glyph(font_desc_t *desc, int c)
  /* renders the glyph corresponding to Unicode character code c and saves the
    image in desc, if it is not there already. */
  {
    FT_GlyphSlot slot;
    FT_UInt glyph_index;
    FT_Glyph oglyph;
    FT_BitmapGlyph glyph;
    int width, height, stride, maxw, off;
    unsigned char *abuffer, *bbuffer;
    int const load_flags = FT_LOAD_NO_HINTING | FT_LOAD_MONOCHROME | FT_LOAD_RENDER;
  /* int const load_flags = FT_LOAD_NO_HINTING; */ /* Anti-aliasing */
    int pen_xa;
    const int font = desc->font[c];
    raw_file * const pic_a = desc->pic_a[font];
    raw_file * const pic_b = desc->pic_b[font];
    int error;
//  fprintf(stderr, "render_one_glyph %d\n", c);
    if (!desc->dynamic)
        return;
    if (desc->width[c] != -1) /* already rendered */
        return;
    if (desc->font[c] == -1)
        return;
    glyph_index = desc->glyph_index[c];
    // load glyph
    error = FT_Load_Glyph(desc->faces[font], glyph_index, load_flags);
    if (error)
      {
        WARNING("FT_Load_Glyph 0x%02x (char 0x%04x) failed.", glyph_index, c);
        desc->font[c] = -1;
        return;
      } /*if*/
    slot = desc->faces[font]->glyph;
    // render glyph
    if (slot->format != ft_glyph_format_bitmap)
      {
      /* make it a bitmap */
        error = FT_Render_Glyph(slot, FT_RENDER_MODE_NORMAL);
        if (error)
          {
            WARNING("FT_Render_Glyph 0x%04x (char 0x%04x) failed.", glyph_index, c);
            desc->font[c] = -1;
            return;
          } /*if*/
      } /*if*/
    // extract glyph image
    error = FT_Get_Glyph(slot, &oglyph);
    if (error)
      {
        WARNING("FT_Get_Glyph 0x%04x (char 0x%04x) failed.", glyph_index, c);
        desc->font[c] = -1;
        return;
      } /*if*/
    if (oglyph->format != FT_GLYPH_FORMAT_BITMAP)
      {
        WARNING("FT_Get_Glyph did not return a bitmap glyph.");
        desc->font[c] = -1;
        return;
      } /*if*/
    glyph = (FT_BitmapGlyph)oglyph;
//  fprintf(stderr, "glyph generated\n");
    maxw = pic_b->charwidth;
    if (glyph->bitmap.width > maxw)
      {
        fprintf(stderr, "WARN: glyph too wide!\n");
      } /*if*/
    // allocate new memory, if needed
//  fprintf(stderr, "\n%d %d %d\n", pic_b->charwidth, pic_b->charheight, pic_b->current_alloc);
    off =
            pic_b->current_count
        *
            pic_b->charwidth
        *
            pic_b->charheight;
    if (pic_b->current_count == pic_b->current_alloc)
      { /* filled allocated space for bmp blocks, need more */
        const size_t ALLOC_INCR = 32; /* grow in steps of this */
        const int newsize =
                pic_b->charwidth
            *
                pic_b->charheight
            *
                (pic_b->current_alloc + ALLOC_INCR);
        const int increment =
                pic_b->charwidth
            *
                pic_b->charheight
            *
                ALLOC_INCR;
        pic_b->current_alloc += ALLOC_INCR;
    //  fprintf(stderr, "\nns = %d inc = %d\n", newsize, increment);
        pic_b->bmp = realloc(pic_b->bmp, newsize);
        pic_a->bmp = realloc(pic_a->bmp, newsize);
      /* initialize newly-added memory to zero: */
        memset(pic_b->bmp + off, 0, increment);
        memset(pic_a->bmp + off, 0, increment);
      } /*if*/
    abuffer = pic_a->bmp + off;
    bbuffer = pic_b->bmp + off;
    paste_bitmap /* copy glyph into next available space in pic_b->bmp */
      (
        /*bbuffer =*/ bbuffer,
        /*bitmap =*/ &glyph->bitmap,
        /*x =*/ pic_b->padding + glyph->left,
        /*y =*/ pic_b->baseline - glyph->top,
        /*width =*/ pic_b->charwidth,
        /*height =*/ pic_b->charheight,
        /*bwidth =*/ glyph->bitmap.width <= maxw ? glyph->bitmap.width : maxw
      );
//  fprintf(stderr, "glyph pasted\n");
    FT_Done_Glyph((FT_Glyph)glyph);
  /* advance pen */
    pen_xa = f266ToInt(slot->advance.x) + 2 * pic_b->padding;
    if (pen_xa > maxw)
        pen_xa = maxw;
    desc->start[c] = off;
    width = desc->width[c] = pen_xa;
    height = pic_b->charheight;
    stride = pic_b->w;
    if (desc->tables.o_r == 0)
      {
        outline0(bbuffer, abuffer, width, height, stride);
      }
    else if (desc->tables.o_r == 1)
      {
        outline1(bbuffer, abuffer, width, height, stride);
      }
    else
      {
        outline
          (
            bbuffer,
            abuffer,
            width,
            height,
            stride,
            desc->tables.omt,
            desc->tables.o_r,
            desc->tables.o_w,
            desc->tables.o_size
          );
      } /*if*/
//  fprintf(stderr, "fg: outline t = %lf\n", GetTimer()-t);
    if (desc->tables.g_r)
      {
        blur
          (
            abuffer,
            desc->tables.tmp,
            width,
            height,
            stride,
            desc->tables.gt2,
            desc->tables.g_r,
            desc->tables.g_w
          );
//      fprintf(stderr, "fg: blur t = %lf\n", GetTimer() - t);
      } /*if*/
    resample_alpha(abuffer, bbuffer, width, height, stride, font_factor);
    pic_b->current_count++;
  } /*render_one_glyph*/

static int prepare_font
  (
    font_desc_t *desc,
    FT_Face face,
    float ppem,
    int pic_idx, /* where in desc->faces to save face */
    int charset_size,
    const FT_ULong *charset,
    const FT_ULong *charcodes,
    int unicode,
    double thickness,
    double radius
  )
  {
    int i, err;
    const int padding = ceil(radius) + ceil(thickness);
    raw_file * pic_a, * pic_b;
    desc->faces[pic_idx] = face;
    pic_a = (raw_file *)malloc(sizeof(raw_file));
    if (pic_a == NULL)
        return -1;
    pic_b = (raw_file *)malloc(sizeof(raw_file));
    if (pic_b == NULL)
      {
        free(pic_a);
        return -1;
      } /*if*/
    desc->pic_a[pic_idx] = pic_a;
    desc->pic_b[pic_idx] = pic_b;
    pic_a->bmp = NULL;
    pic_a->pal = NULL;
    pic_b->bmp = NULL;
    pic_b->pal = NULL;
    pic_a->pal = (unsigned char *)malloc(sizeof(unsigned char) * 256 * 3);
    if (!pic_a->pal)
        return -1;
    for (i = 0; i < 768; ++i)
        pic_a->pal[i] = i / 3; /* fill with greyscale ramp */
    pic_b->pal = (unsigned char *)malloc(sizeof(unsigned char) * 256 * 3);
    if (!pic_b->pal)
        return -1;
    for (i = 0; i < 768; ++i)
        pic_b->pal[i] = i / 3; /* fill with greyscale ramp */
//  ttime = GetTimer();
    err = check_font(desc, ppem, padding, pic_idx, charset_size, charset, charcodes, unicode);
//  ttime = GetTimer() - ttime;
//  printf("render:   %7lf us\n", ttime);
    if (err)
        return -1;
//  fprintf(stderr, "fg: render t = %lf\n", GetTimer() - t);
    pic_a->w = pic_b->w;
    pic_a->h = pic_b->h;
    pic_a->c = colors;
    pic_a->bmp = NULL;
//  fprintf(stderr, "fg: w = %d, h = %d\n", pic_a->w, pic_a->h);
    return 0;
  } /*prepare_font*/

static int generate_tables
  (
    font_desc_t *desc,
    double thickness,
    double radius
  )
  {
    const int width = desc->max_height;
    const int height = desc->max_width;
    const double A = log(1.0 / base) / (radius * radius * 2);
    int mx, my, i;
    double volume_diff, volume_factor = 0;
    unsigned char *omtp;
    desc->tables.g_r = ceil(radius);
    desc->tables.o_r = ceil(thickness);
    desc->tables.g_w = 2 * desc->tables.g_r + 1;
    desc->tables.o_w = 2 * desc->tables.o_r + 1;
    desc->tables.o_size = desc->tables.o_w * desc->tables.o_w;
//  fprintf(stderr, "o_r = %d\n", desc->tables.o_r);
    if (desc->tables.g_r)
      {
        desc->tables.g = (unsigned *)malloc(desc->tables.g_w * sizeof(unsigned));
        desc->tables.gt2 = (unsigned *)malloc(256 * desc->tables.g_w * sizeof(unsigned));
        if (desc->tables.g == NULL || desc->tables.gt2 == NULL)
          {
            return -1;
          } /*if*/
      } /*if*/
    desc->tables.om = (unsigned *)malloc(desc->tables.o_w * desc->tables.o_w * sizeof(unsigned));
    desc->tables.omt = malloc(desc->tables.o_size * 256);
    omtp = desc->tables.omt;
    desc->tables.tmp = malloc((width + 1) * height * sizeof(short));
    if (desc->tables.om == NULL || desc->tables.omt == NULL || desc->tables.tmp == NULL)
      {
        return -1;
      }; /*if*/
    if (desc->tables.g_r)
      {
        // gaussian curve with volume = 256
        for (volume_diff = 10000000; volume_diff > 0.0000001; volume_diff *= 0.5)
          {
            volume_factor += volume_diff;
            desc->tables.volume = 0;
            for (i = 0; i < desc->tables.g_w; ++i)
              {
                desc->tables.g[i] = (unsigned)
                    (exp(A * (i - desc->tables.g_r) * (i - desc->tables.g_r)) * volume_factor + .5);
                desc->tables.volume += desc->tables.g[i];
              } /*for*/
            if (desc->tables.volume > 256)
                volume_factor -= volume_diff;
          } /*for*/
        desc->tables.volume = 0;
        for (i = 0; i < desc->tables.g_w; ++i)
          {
            desc->tables.g[i] = (unsigned)
                (exp(A * (i - desc->tables.g_r) * (i - desc->tables.g_r)) * volume_factor + .5);
            desc->tables.volume += desc->tables.g[i];
          } /*for*/

        // gauss table:
        for (mx = 0; mx < desc->tables.g_w; mx++)
          {
            for (i = 0; i < 256; i++)
              {
                desc->tables.gt2[mx + i * desc->tables.g_w] = i * desc->tables.g[mx];
              } /*for*/
          } /*for*/
      } /*if*/
  /* outline matrix */
    for (my = 0; my < desc->tables.o_w; ++my)
      {
        for (mx = 0; mx < desc->tables.o_w; ++mx)
          {
          // antialiased circle would be perfect here, but this one is good enough
            double d =
                    thickness
                +
                    1
                 -
                    sqrt
                      (
                            (mx - desc->tables.o_r) * (mx - desc->tables.o_r)
                        +
                            (my - desc->tables.o_r) * (my - desc->tables.o_r)
                      );
            desc->tables.om[mx + my * desc->tables.o_w] =
                d >= 1 ?
                    base
                : d <= 0 ?
                    0
                :
                    d * base + .5;
          } /*for*/
      } /*for*/
  // outline table:
    for (i = 0; i < 256; i++)
      {
        for (mx = 0; mx < desc->tables.o_size; mx++)
            *omtp++ = (i * desc->tables.om[mx] + base / 2) / base;
      } /*for*/
    return 0;
  } /*generate_tables*/

/* decode from 'encoding' to unicode */
static FT_ULong decode_char(const iconv_t *cd, char c)
  {
    FT_ULong o;
    char *inbuf = &c;
    char *outbuf = (char*)&o;
    size_t inbytesleft = 1;
    size_t outbytesleft = sizeof(FT_ULong);
    iconv(*cd, &inbuf, &inbytesleft, &outbuf, &outbytesleft);
    o = ntohl(o);
    // if (count == -1) o = 0; // not OK, at least my iconv() returns E2BIG for all
    if (outbytesleft != 0)
        o = 0;
  /* we don't want control characters */
    if (o >= 0x7f && o < 0xa0)
        o = 0;
    return o;
  } /*decode_char*/

static int prepare_charset
  (
    const char *charmap, /* destination encoding, actually always UCS-4 */
    const char *encoding, /* source encoding, must be 8-bit */
    FT_ULong *charset, /* array [256 - first_char], filled in with [33 .. 255] */
    FT_ULong *charcodes
      /* array [256 - first_char], translation of corresponding charset codes from
        charmap to encoding */
  )
  /* fills in charset with the values [33 .. 255] in sequence, and charcodes with
    the corresponding character codes when translating these from encoding to charmap. */
  {
    FT_ULong i;
    int count = 0;
    int charset_size;
    iconv_t cd;
    // check if ucs-4 is available
    cd = iconv_open(charmap, charmap);
    if (cd == (iconv_t)-1)
      {
        fprintf(stderr,"ERR: Iconv doesn't know %s encoding. Use the source!\n", charmap);
        return -1;
      } /*if*/
    iconv_close(cd);
    cd = iconv_open(charmap, encoding);
    if (cd == (iconv_t)-1)
      {
        fprintf
          (
            stderr,
            "ERR: Unsupported encoding `%s', use iconv --list to list"
                " character sets known on your system.\n",
            encoding
          );
        return -1;
      } /*if*/
    charset_size = 256 - first_char;
    for (i = 0; i < charset_size; ++i)
      {
        charcodes[count] = i + first_char;
        charset[count] = decode_char(&cd, i + first_char);
        if (charset[count] != 0)
            ++count;
      } /*for*/
    charcodes[count] = charset[count] = 0;
    ++count;
    charset_size = count;
    iconv_close(cd);
    if (charset_size == 0)
      {
        fprintf(stderr, "ERR: No characters to render!\n");
        return -1;
      } /*if*/
    return charset_size;
  } /*prepare_charset*/

static int prepare_charset_unicode
  (
    FT_Face face,
    FT_ULong *charset,
      /* filled in with all the character codes for which glyphs are defined in the font */
    FT_ULong *charcodes
      /* filled in with zeroes to the same length as charset */
  )
  {
#ifdef HAVE_FREETYPE21
    FT_ULong charcode;
#else
    int j;
#endif
    FT_UInt gindex;
    int i;
    if (face->charmap == NULL || face->charmap->encoding != FT_ENCODING_UNICODE)
      {
        WARNING("Unicode charmap not available for this font. Very bad!");
        return -1;
      } /*if*/
#ifdef HAVE_FREETYPE21
    i = 0;
    charcode = FT_Get_First_Char(face, &gindex);
    while (gindex != 0)
      {
        if (charcode < 65536 && charcode >= 33) // sanity check
          {
            charset[i] = charcode;
            charcodes[i] = 0;
            i++;
          } /*if*/
        charcode = FT_Get_Next_Char(face, charcode, &gindex);
      } /*while*/
#else
  // for FT < 2.1 we have to use brute force enumeration
    i = 0;
    for (j = 33; j < 65536; j++)
      {
        gindex = FT_Get_Char_Index(face, j);
        if (gindex > 0)
          {
            charset[i] = j;
            charcodes[i] = 0;
            i++;
          } /*if*/
      } /*for*/
#endif
    fprintf(stderr, "INFO: Unicode font: %d glyphs.\n", i);
    return i;
  } /*prepare_charset_unicode*/

static font_desc_t* init_font_desc()
  /* allocates and initializes a new font_desc_t structure. */
  {
    font_desc_t *desc;
    int i;
    desc = malloc(sizeof(font_desc_t));
    if (!desc)
        return NULL;
    memset(desc, 0, sizeof(font_desc_t));
    desc->dynamic = 1;
  /* setup sane defaults, mark all associated storage as unallocated */
    desc->name = NULL;
    desc->fpath = NULL;
    desc->face_cnt = 0;
    desc->charspace = 0;
    desc->spacewidth = 0;
    desc->height = 0;
    desc->max_width = 0;
    desc->max_height = 0;
    desc->tables.g = NULL;
    desc->tables.gt2 = NULL;
    desc->tables.om = NULL;
    desc->tables.omt = NULL;
    desc->tables.tmp = NULL;
    for (i = 0; i < 65536; i++)
        desc->start[i] = desc->width[i] = desc->font[i] = -1;
    for (i = 0; i < 16; i++)
        desc->pic_a[i] = desc->pic_b[i] = NULL;
    return desc;
  } /*init_font_desc*/

void free_font_desc(font_desc_t *desc)
  /* disposes of all storage allocated for a font_desc_t structure. */
  {
    int i;
    if (!desc)
        return; /* nothing to do */
//  if (!desc->dynamic) return; // some vo_aa crap, better leaking than crashing
    free(desc->name);
    free(desc->fpath);
    for (i = 0; i < 16; i++)
      {
        if (desc->pic_a[i])
          {
            free(desc->pic_a[i]->bmp);
            free(desc->pic_a[i]->pal);
          } /*if*/
        if (desc->pic_b[i])
          {
            free(desc->pic_b[i]->bmp);
            free(desc->pic_b[i]->pal);
          } /*if*/
        free(desc->pic_a[i]);
        free(desc->pic_b[i]);
      } /*for*/
    free(desc->tables.g);
    free(desc->tables.gt2);
    free(desc->tables.om);
    free(desc->tables.omt);
    free(desc->tables.tmp);
    for (i = 0; i < desc->face_cnt; i++)
      {
        FT_Done_Face(desc->faces[i]);
      } /*for*/
    free(desc);
  } /*free_font_desc*/

static int load_sub_face(const char *name, FT_Face *face)
  {
    int err = -1;
    if (name)
        err = FT_New_Face(library, name, 0, face);
    if (err)
      {
        const char * const fontpath = get_config_path(sub_font);
        err = FT_New_Face(library, fontpath, 0, face);
        free((void *)fontpath);
        if (err)
          {
#if 0
            char *mp_sub_font;
            sprintf(mp_sub_font, TEXTSUB_DATADIR "%s", sub_font);
            err = FT_New_Face(library, mp_sub_font, 0, face);
            if (err)
#endif
              {
                fprintf
                  (
                    stderr,
                    "ERR: New_Face failed. Maybe the font path is wrong.\n"
                        "Please supply the text font file (%s).\n",
                    sub_font
                  );
                return -1;
              } /*if*/
          } /*if*/
      } /*if*/
    return err;
  } /*load_sub_face*/

int kerning(font_desc_t *desc, int prevc, int c)
  /* returns the amount of kerning to apply between character c and previous character prevc. */
  {
    FT_Vector kern;
    if (!vo_font->dynamic)
        return 0;
    if (prevc < 0 || c < 0) /* need 2 characters to kern */
        return 0;
    if (desc->font[prevc] != desc->font[c]) /* font change => don't kern */
        return 0;
    if (desc->font[prevc] == -1 /* <=> desc->font[c] == -1 */)
        return 0;
    FT_Get_Kerning
      (
        desc->faces[desc->font[c]],
        desc->glyph_index[prevc],
        desc->glyph_index[c],
        ft_kerning_default,
        &kern
      );
//  fprintf(stderr, "kern: %c %c %d\n", prevc, c, f266ToInt(kern.x));
    return f266ToInt(kern.x);
  } /*kerning*/

font_desc_t* read_font_desc_ft
  (
    const char *fname,
    int movie_width,
    int movie_height
  )
  {
    font_desc_t *desc;
    FT_Face face;
    FT_ULong my_charset[MAX_CHARSET_SIZE]; /* characters we want to render; Unicode */
    FT_ULong my_charcodes[MAX_CHARSET_SIZE]; /* character codes in 'encoding' */
    const char * const charmap = "ucs-4";
    int err;
    int charset_size;
    int i, j;
    int unicode;
    float movie_size;
    float subtitle_font_ppem;
    float osd_font_ppem;
    switch (subtitle_autoscale)
      {
    case AUTOSCALE_MOVIE_HEIGHT:
        movie_size = movie_height;
    break;
    case AUTOSCALE_MOVIE_WIDTH:
        movie_size = movie_width;
    break;
    case AUTOSCALE_MOVIE_DIAGONAL:
        movie_size = sqrt(movie_height * movie_height + movie_width * movie_width);
    break;
    case AUTOSCALE_NONE:
    default:
        movie_size = 100;
    break;
      } /*switch*/
    subtitle_font_ppem = movie_size * text_font_scale_factor / 100.0;
    osd_font_ppem = movie_size * osd_font_scale_factor / 100.0;
    if (subtitle_font_ppem < 5)
        subtitle_font_ppem = 5;
    if (osd_font_ppem < 5)
        osd_font_ppem = 5;
    if (subtitle_font_ppem > 128)
        subtitle_font_ppem = 128;
    if (osd_font_ppem > 128)
        osd_font_ppem = 128;
    unicode =
            subtitle_font_encoding == NULL
        ||
            strcasecmp(subtitle_font_encoding, "unicode") == 0;
    desc = init_font_desc();
    if (!desc)
        return NULL;
//  t = GetTimer();
  /* generate the subtitle font */
    err = load_sub_face(fname, &face);
    if (err)
      {
        fprintf(stderr, "WARN: subtitle font: load_sub_face failed.\n");
        goto skip_a_part;
      } /*if*/
    desc->face_cnt++; /* will always be 1, since I just created desc */
    if (unicode)
      {
        charset_size = prepare_charset_unicode(face, my_charset, my_charcodes);
      }
    else
      {
        charset_size = prepare_charset
          (
            charmap, /* always UCS-4 */
            subtitle_font_encoding != NULL ?
                subtitle_font_encoding
            :
                "iso-8859-1" /* fixme: use locale default? */,
            my_charset,
            my_charcodes
          );
      } /*if*/
    if (charset_size < 0)
      {
        fprintf(stderr, "ERR: subtitle font: prepare_charset failed.\n");
        free_font_desc(desc);
        return NULL;
      } /*if*/
//  fprintf(stderr, "fg: prepare t = %lf\n", GetTimer() - t);
    err = prepare_font
      (
        /*desc =*/ desc,
        /*face =*/ face,
        /*ppem =*/ subtitle_font_ppem,
        /*pic_idx =*/ desc->face_cnt - 1,
        /*charset_size =*/ charset_size,
        /*charset =*/ my_charset,
        /*charcodes =*/ my_charcodes,
        /*unicode =*/ unicode,
        /*thickness =*/ subtitle_font_thickness,
        /*radius =*/ subtitle_font_radius
      );
    if (err)
      {
        fprintf(stderr, "ERR: Cannot prepare subtitle font.\n");
        free_font_desc(desc);
        return NULL;
      } /*if*/
skip_a_part:
    err = generate_tables(desc, subtitle_font_thickness, subtitle_font_radius);
    if (err)
      {
        fprintf(stderr, "ERR: Cannot generate tables.\n");
        free_font_desc(desc);
        return NULL;
      } /*if*/
    // final cleanup
    desc->font[' '] = -1;
    desc->width[' '] = desc->spacewidth;
    j = '_';
    if (desc->font[j] < 0)
        j = '?';
    if (desc->font[j] < 0)
        j = ' ';
    render_one_glyph(desc, j);
    for (i = 0; i < 65536; i++)
      {
        if (i != ' ' && desc->font[i] < 0)
          {
            desc->start[i] = desc->start[j];
            desc->width[i] = desc->width[j];
            desc->font[i] = desc->font[j];
          } /*if*/
      } /*for*/
    return desc;
  } /*read_font_desc_ft*/

int init_freetype()
  {
    int err;
  /* initialize freetype */
    err = FT_Init_FreeType(&library);
    if (err)
      {
        fprintf(stderr, "ERR: Init_FreeType failed.\n");
        return -1;
      } /*if*/
/*  fprintf(stderr, "INFO: init_freetype\n"); */
    using_freetype = 1;
    return 0;
  } /*init_freetype*/

int done_freetype()
  {
    int err;
    if (!using_freetype)
        return 0;
    err = FT_Done_FreeType(library);
    if (err)
      {
        fprintf(stderr, "ERR: FT_Done_FreeType failed.\n");
        return -1;
      } /*if*/
    return 0;
  } /*done_freetype*/

void load_font_ft(int width, int height)
  {
    vo_image_width = width;
    vo_image_height = height;
    // protection against vo_aa font hacks
    if (vo_font && !vo_font->dynamic)
        return;
    free_font_desc(vo_font);
    vo_font = read_font_desc_ft(textsub_font_name, width, height);
  } /*load_font_ft*/

#endif /* HAVE_FREETYPE */
