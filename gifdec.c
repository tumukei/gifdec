#include "gifdec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MIN(A, B) ((A) < (B) ? (A) : (B))
#define MAX(A, B) ((A) > (B) ? (A) : (B))

typedef struct Entry {
    uint16_t length;
    uint16_t prefix;
    uint8_t  suffix;
} Entry;

typedef struct Table {
    int bulk;
    int nentries;
    Entry *entries;
} Table;

static uint16_t
read_num(FILE *fd)
{
    uint8_t bytes[2];

    fread(bytes, 2, 1, fd);
    return bytes[0] + (((uint16_t) bytes[1]) << 8);
}

/* Open GIF file
 * Return pointer to gd_GIF if success or NULL on failure.
*/
gd_GIF *
gd_open_gif(const char *fname, int canvasdepth)
{
    FILE *fd;
    uint8_t sigver[6];
    uint16_t width, height, depth;
    int fdsz, bgidx, aspect;
    int gct_sz;
    gd_GIF *gif = NULL;
    size_t sz;
    int canvasbytes;

    fd = fopen(fname, "rb");
    if (!fd)
        return NULL;

    /* Header */
    sz = fread(sigver, 6, 1, fd);

    if (sz != 1)
        goto fail;
    if (memcmp(sigver, "GIF", 3) != 0) {
        goto fail;
    }

    if (memcmp(sigver+3, "89a", 3) != 0) {
        goto fail;
    }

    /* Width x Height */
    width  = read_num(fd);
    height = read_num(fd);
    /* FDSZ */
    fdsz = fgetc(fd);
    /* Presence of GCT */
    if (!(fdsz & 0x80)) {
        goto fail;
    }
    /* Color Space's Depth */
    depth = ((fdsz >> 4) & 7) + 1;
    /* Ignore Sort Flag. */
    /* GCT Size */
    gct_sz = 1 << ((fdsz & 0x07) + 1);
    /* Background Color Index */
    bgidx = fgetc(fd);
    /* Aspect Ratio */
    aspect = fgetc(fd);
    /* Create gd_GIF Structure. */
    if (canvasdepth > 16)
        canvasbytes = 3;
    else if (canvasdepth > 8)
        canvasbytes = 2;
    else
        canvasbytes = 1;

    gif->canvasbytes = canvasbytes;
    gif = calloc(1, sizeof(*gif) + (width * height) + (canvasbytes * width * height));
    if (!gif) goto fail;
    gif->fd = fd;
    gif->width  = width;
    gif->height = height;
    gif->depth  = depth;
    /* Read GCT */
    gif->gct.size = gct_sz;
    sz = fread(gif->gct.colors, 3, gct_sz, fd);
    if (sz != gct_sz)
        goto fail_gct;
    gif->palette = &gif->gct;
    gif->bgindex = bgidx;
    gif->canvas = (uint8_t *) &gif[1];
    gif->frame = &gif->canvas[canvasbytes * width * height];
    if (gif->bgindex)
        memset(gif->frame, gif->bgindex, gif->width * gif->height);
    gif->anim_start = ftell(fd);
    
    goto ok;
fail_gct:
    free(gif);
    gif = NULL;
fail:
    fclose(fd);
ok:
    return gif;
}

/* read until subblocks end */
static void
discard_sub_blocks(gd_GIF *gif)
{
    int size;
    char dummy[256];

    do {
        size = fgetc(gif->fd);
        fread(&dummy, size, 1, gif->fd);
    } while (size);
}

static void
read_plain_text_ext(gd_GIF *gif)
{
    if (gif->plain_text) {
        uint16_t tx, ty, tw, th;
        uint8_t whfb[4];
        int sub_block;
        int dummy;
        /* skip block size */
        dummy = fgetc(gif->fd);  /* block size = 12 */

        tx = read_num(gif->fd);
        ty = read_num(gif->fd);
        tw = read_num(gif->fd);
        th = read_num(gif->fd);
        fread(&whfb, 4, 1, gif->fd);
        sub_block = ftell(gif->fd);
        gif->plain_text(gif, tx, ty, tw, th, whfb[0], whfb[1], whfb[2], whfb[3]);
        fseek(gif->fd, sub_block, SEEK_SET);
    } else {
        char dummy[13];
        /* Discard plain text metadata. */
        fread(&dummy, 13, 1, gif->fd);
    }
    /* Discard plain text sub-blocks. */
    discard_sub_blocks(gif);
}

static void
read_graphic_control_ext(gd_GIF *gif)
{
    int rdit;

    /* Discard block size (always 0x04). */
    rdit = fgetc(gif->fd);
    rdit = fgetc(gif->fd);
    gif->gce.disposal = (rdit >> 2) & 3;
    gif->gce.input = rdit & 2;
    gif->gce.transparency = rdit & 1;

    gif->gce.delay = read_num(gif->fd);

    gif->gce.tindex = fgetc(gif->fd);
    /* Skip block terminator. */
    rdit = fgetc(gif->fd);
}

static void
read_comment_ext(gd_GIF *gif)
{
    if (gif->comment) {
        int sub_block;
        sub_block = ftell(gif->fd);
        gif->comment(gif);
        fseek(gif->fd, sub_block, SEEK_SET);
    }
    /* Discard comment sub-blocks. */
    discard_sub_blocks(gif);
}

static void
read_application_ext(gd_GIF *gif)
{
    char app_id[8];
    char app_auth_code[3];
    int dummy;

    /* Discard block size (always 0x0B). */
    dummy = fgetc(gif->fd);
    /* Application Identifier. */
    fread(app_id, 8, 1, gif->fd);
    /* Application Authentication Code. */
    fread(app_auth_code, 3, 1, gif->fd);
    if (!memcmp(app_id, "NETSCAPE", sizeof(app_id))) {
        /* Discard block size (0x03) and constant byte (0x01). */
        dummy = fgetc(gif->fd);
        dummy = fgetc(gif->fd);
        gif->loop_count = read_num(gif->fd);
        /* Skip block terminator. */
        dummy = fgetc(gif->fd);

    } else if (gif->application) {
        int sub_block;
        sub_block = ftell(gif->fd);
        gif->application(gif, app_id, app_auth_code);
        fseek(gif->fd, sub_block, SEEK_SET);
        discard_sub_blocks(gif);
    } else {
        discard_sub_blocks(gif);
    }
}

static void
read_ext(gd_GIF *gif)
{
    int label;

    label = fgetc(gif->fd);
    switch (label) {
    case 0x01:
        read_plain_text_ext(gif);
        break;
    case 0xF9:
        read_graphic_control_ext(gif);
        break;
    case 0xFE:
        read_comment_ext(gif);
        break;
    case 0xFF:
        read_application_ext(gif);
        break;
    default:
        fprintf(stderr, "unknown extension: %02X\n", label);
        break;
    }
}

static Table *
new_table(int key_size)
{
    int key;
    int init_bulk = MAX(1 << (key_size + 1), 0x100);
    Table *table = malloc(sizeof(*table) + sizeof(Entry) * init_bulk);
    if (table) {
        table->bulk = init_bulk;
        table->nentries = (1 << key_size) + 2;
        table->entries = (Entry *) &table[1];
        for (key = 0; key < (1 << key_size); key++)
            table->entries[key] = (Entry) {1, 0xFFF, key};
    }
    return table;
}

/* Add table entry. Return value:
 *  0 on success
 *  +1 if key size must be incremented after this addition
 *  -1 if could not realloc table */
static int
add_entry(Table **tablep, uint16_t length, uint16_t prefix, uint8_t suffix)
{
    Table *table = *tablep;
    if (table->nentries == table->bulk) {
        table->bulk *= 2;
        table = realloc(table, sizeof(*table) + sizeof(Entry) * table->bulk);
        if (!table) return -1;
        table->entries = (Entry *) &table[1];
        *tablep = table;
    }
    table->entries[table->nentries] = (Entry) {length, prefix, suffix};
    table->nentries++;
    if ((table->nentries & (table->nentries - 1)) == 0)
        return 1;
    return 0;
}

static uint16_t
get_key(gd_GIF *gif, int key_size, uint8_t *sub_len, uint8_t *shift, uint8_t *byte)
{
    int bits_read;
    int rpad;
    int frag_size;
    uint16_t key;

    key = 0;
    for (bits_read = 0; bits_read < key_size; bits_read += frag_size) {
        rpad = (*shift + bits_read) % 8;
        if (rpad == 0) {
            /* Update byte. */
            if (*sub_len == 0)
                *sub_len = fgetc(gif->fd); /* Must be nonzero! */
            *byte = fgetc(gif->fd);
            (*sub_len)--;
        }
        frag_size = MIN(key_size - bits_read, 8 - rpad);
        key |= ((uint16_t) ((*byte) >> rpad)) << bits_read;
    }
    /* Clear extra bits to the left. */
    key &= (1 << key_size) - 1;
    *shift = (*shift + key_size) % 8;
    return key;
}

/* Compute output index of y-th input line, in frame of height h. */
static int
interlaced_line_index(int h, int y)
{
    int p; /* number of lines in current pass */

    p = (h - 1) / 8 + 1;
    if (y < p) /* pass 1 */
        return y * 8;
    y -= p;
    p = (h - 5) / 8 + 1;
    if (y < p) /* pass 2 */
        return y * 8 + 4;
    y -= p;
    p = (h - 3) / 4 + 1;
    if (y < p) /* pass 3 */
        return y * 4 + 2;
    y -= p;
    /* pass 4 */
    return y * 2 + 1;
}

/* Decompress image pixels.
 * Return 0 on success or -1 on out-of-memory (w.r.t. LZW code table). */
static int
read_image_data(gd_GIF *gif, int interlace)
{
    uint8_t sub_len, shift, byte;
    int init_key_size, key_size, table_is_full;
    int frm_off, str_len, p, x, y;
    uint16_t key, clear, stop;
    int ret;
    Table *table;
    Entry entry;
    int start, end;

    byte = fgetc(gif->fd);
    key_size = (int) byte;
    start = ftell(gif->fd);
    discard_sub_blocks(gif);
    end = ftell(gif->fd);
    fseek(gif->fd, start, SEEK_SET);
    clear = 1 << key_size;
    stop = clear + 1;
    table = new_table(key_size);
    if (!table)
        return -1;
    key_size++;
    init_key_size = key_size;
    sub_len = shift = 0;
    key = get_key(gif, key_size, &sub_len, &shift, &byte); /* clear code */
    frm_off = 0;
    ret = 0;
    while (1) {
        if (key == clear) {
            key_size = init_key_size;
            table->nentries = (1 << (key_size - 1)) + 2;
            table_is_full = 0;
        } else if (!table_is_full) {
            ret = add_entry(&table, str_len + 1, key, entry.suffix);
            if (ret == -1) {
                free(table);
                return -1;
            }
            if (table->nentries == 0x1000) {
                ret = 0;
                table_is_full = 1;
            }
        }
        key = get_key(gif, key_size, &sub_len, &shift, &byte);
        if (key == clear) continue;
        if (key == stop) break;
        if (ret == 1) key_size++;
        entry = table->entries[key];
        str_len = entry.length;
        while (1) {
            p = frm_off + entry.length - 1;
            x = p % gif->fw;
            y = p / gif->fw;
            if (interlace)
                y = interlaced_line_index((int) gif->fh, y);
            gif->frame[(gif->fy + y) * gif->width + gif->fx + x] = entry.suffix;
            if (entry.prefix == 0xFFF)
                break;
            else
                entry = table->entries[entry.prefix];
        }
        frm_off += str_len;
        if (key < table->nentries - 1 && !table_is_full)
            table->entries[table->nentries - 1].suffix = entry.suffix;
    }
    free(table);
    sub_len = fgetc(gif->fd); /* Must be zero! */

    fseek(gif->fd, end, SEEK_SET);
    return 0;
}

/* Read image.
 * Return 0 on success or -1 on out-of-memory (w.r.t. LZW code table). */
static int
read_image(gd_GIF *gif)
{
    int fisrz;
    int interlace;
    int ret;

    /* Image Descriptor. */
    gif->fx = read_num(gif->fd);
    gif->fy = read_num(gif->fd);
    gif->fw = read_num(gif->fd);
    gif->fh = read_num(gif->fd);
    fisrz = fgetc(gif->fd);
    interlace = fisrz & 0x40;
    /* Ignore Sort Flag. */
    /* Local Color Table? */
    if (fisrz & 0x80) {
        /* Read LCT */
        gif->lct.size = 1 << ((fisrz & 0x07) + 1);
        fread(gif->lct.colors, 3, gif->lct.size, gif->fd);
        gif->palette = &gif->lct;
    } else
        gif->palette = &gif->gct;
    /* Image Data. */
    ret = read_image_data(gif, interlace);
    return ret;
}

static int reduce_color(uint8_t *rgb, int to)
{
    int rd = 0;
    if (to == 2) {
        /* 565 */
        rd = (0xf8 & rgb[0]) + (rgb[1]>>5);
        rd <<= 8;
        rd += (0x1c & rgb[1])<<3 + (rgb[2]>>3);
    }
    else if (to == 1) {
        /* 332 */
        rd = (0xe0 & rgb[0]) + (0xe0 & rgb[1]>>3) + (0xc0 & rgb[2])>>6;
    }

    return rd;
}

static void
render_frame_rect(gd_GIF *gif, uint8_t *buffer)
{
    int i, j, k;
    uint8_t index, *color;
    i = gif->fy * gif->width + gif->fx;
    for (j = 0; j < gif->fh; j++) {
        for (k = 0; k < gif->fw; k++) {
            index = gif->frame[(gif->fy + j) * gif->width + gif->fx + k];
            color = &gif->palette->colors[index*3];
            if (!gif->gce.transparency || index != gif->gce.tindex)
                if (gif->canvasbytes == 3)
                    memcpy(&buffer[(i+k)*3], color, 3);
                else if (gif->canvasbytes == 2)
                {
                    int col;
                    col = reduce_color(color, 2);
                    buffer[(i+k)*2] = col >> 8;
                    buffer[(i+k)*2 + 1] = col;
                }
                else
                {
                    buffer[(i+k)] = reduce_color(color, 1);
                }

        }
        i += gif->width;
    }
}

static void
dispose(gd_GIF *gif)
{
    int i, j, k;
    uint8_t *bgcolor;
    switch (gif->gce.disposal) {
    case 2: /* Restore to background color. */
        bgcolor = &gif->palette->colors[gif->bgindex*3];
        i = gif->fy * gif->width + gif->fx;
        for (j = 0; j < gif->fh; j++) {
            for (k = 0; k < gif->fw; k++)
                if (gif->canvasbytes == 3)
                    memcpy(&gif->canvas[(i+k)*3], bgcolor, 3);
                else if (gif->canvasbytes == 2) {
                    int col;
                    col = reduce_color(bgcolor, 2);
                    gif->canvas[(i+k)*2] = col >> 8;
                    gif->canvas[(i+k)*2 + 1] = col;
                }
                else {
                    gif->canvas[(i+k)] = reduce_color(bgcolor, 1);
                }
            i += gif->width;
        }
        break;
    case 3: /* Restore to previous, i.e., don't update canvas.*/
        break;
    default:
        /* Add frame non-transparent pixels to canvas. */
        render_frame_rect(gif, gif->canvas);
    }
}

/* Return 1 if got a frame; 0 if got GIF trailer or error. */
int
gd_get_frame(gd_GIF *gif)
{
    char sep;

    dispose(gif);

    do {
        sep = fgetc(gif->fd);

        if (feof(gif->fd))
            return 0;

        if (sep == ';')
            return 0;
        if (sep == '!')
            read_ext(gif);

    } while (sep != ',');

    if (read_image(gif))
        return 0;

    return 1;
}

void
gd_render_frame(gd_GIF *gif, uint8_t *buffer)
{
    memcpy(buffer, gif->canvas, gif->width * gif->height * 3);
    render_frame_rect(gif, buffer);
}

void
gd_rewind(gd_GIF *gif)
{
    fseek(gif->fd, gif->anim_start, SEEK_SET);
}

void
gd_close_gif(gd_GIF *gif)
{
    fclose(gif->fd);
    free(gif);
}
