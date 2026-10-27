#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>

#include "metadata.h"

// longest ID3 text frame we bother reading, longer ones are skipped
#define TEXT_FRAME_MAX 4096

// enough of a cover frame to get past its mime type and description
#define PICTURE_HEADER_MAX 1024

// flac comments are small, unless someone stuffed a cover in there as base64
#define COMMENT_BLOCK_MAX (1024 * 1024)

// picture type for "front cover" in both ID3 and flac
#define FRONT_COVER 3

enum tag { TAG_TITLE, TAG_ARTIST, TAG_ALBUM, TAG_YEAR, TAG_TRACK };

static uint32_t be32(const unsigned char* p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static uint32_t le32(const unsigned char* p) {
    return (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0];
}

// ID3 sizes use 7 bits per byte so they never look like an mp3 sync marker
static uint32_t synchsafe32(const unsigned char* p) {
    return (uint32_t)(p[0] & 0x7f) << 21 | (uint32_t)(p[1] & 0x7f) << 14 |
           (uint32_t)(p[2] & 0x7f) << 7 | (p[3] & 0x7f);
}

// append code point cp to out as UTF-8. -1 if it doesn't fit
static int put_utf8(char* out, size_t size, size_t* len, uint32_t cp) {
    char buf[4];
    size_t n;

    if (cp < 0x80) {
        buf[0] = cp;
        n = 1;
    } else if (cp < 0x800) {
        buf[0] = 0xC0 | cp >> 6;
        buf[1] = 0x80 | (cp & 0x3F);
        n = 2;
    } else if (cp < 0x10000) {
        buf[0] = 0xE0 | cp >> 12;
        buf[1] = 0x80 | (cp >> 6 & 0x3F);
        buf[2] = 0x80 | (cp & 0x3F);
        n = 3;
    } else {
        buf[0] = 0xF0 | cp >> 18;
        buf[1] = 0x80 | (cp >> 12 & 0x3F);
        buf[2] = 0x80 | (cp >> 6 & 0x3F);
        buf[3] = 0x80 | (cp & 0x3F);
        n = 4;
    }

    // keep room for the terminator
    if (*len + n >= size) {
        return -1;
    }

    memcpy(out + *len, buf, n);
    *len += n;
    return 0;
}

// copy UTF-8 text into out, stopping at a NUL or when full without
// cutting a multi-byte character in half
static void copy_utf8(char* out, size_t size, const char* text, size_t len) {
    size_t n = 0;

    while (n < len && n + 1 < size && text[n] != '\0') {
        out[n] = text[n];
        n++;
    }

    // if we stopped early, drop a trailing character that didn't fully fit
    if (n < len && text[n] != '\0') {
        while (n > 0 && ((unsigned char)text[n] & 0xC0) == 0x80) {
            n--;
        }
    }

    out[n] = '\0';
}

// decode an ID3 string in encoding enc to UTF-8, stopping at the first terminator
static void id3_text(const unsigned char* data, size_t len, int enc, char* out, size_t size) {
    size_t out_len = 0;

    if (enc == 3) {
        copy_utf8(out, size, (const char*)data, len);
        return;
    }

    if (enc == 0) {
        // ISO-8859-1, every byte is its own code point
        for (size_t i = 0; i < len && data[i] != 0; i++) {
            if (put_utf8(out, size, &out_len, data[i]) == -1) {
                break;
            }
        }
    } else if (enc == 1 || enc == 2) {
        // UTF-16, with a byte order mark for encoding 1 and big endian for 2
        int big_endian = enc == 2;
        size_t i = 0;

        if (enc == 1 && len >= 2) {
            if (data[0] == 0xFE && data[1] == 0xFF) {
                big_endian = 1;
                i = 2;
            } else if (data[0] == 0xFF && data[1] == 0xFE) {
                i = 2;
            }
        }

        for (; i + 1 < len; i += 2) {
            uint32_t unit = big_endian ? data[i] << 8 | data[i + 1] : data[i + 1] << 8 | data[i];
            uint32_t cp = unit;

            if (unit == 0) {
                break;
            }

            // characters outside the basic plane come as a surrogate pair
            if (unit >= 0xD800 && unit <= 0xDBFF && i + 3 < len) {
                uint32_t low = big_endian ? data[i + 2] << 8 | data[i + 3] : data[i + 3] << 8 | data[i + 2];

                if (low >= 0xDC00 && low <= 0xDFFF) {
                    cp = 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00);
                    i += 2;
                } else {
                    cp = 0xFFFD;
                }
            } else if (unit >= 0xD800 && unit <= 0xDFFF) {
                cp = 0xFFFD;
            }

            if (put_utf8(out, size, &out_len, cp) == -1) {
                break;
            }
        }
    }

    out[out_len] = '\0';
}

// fill in a field, first value found wins
static void set_field(char* field, size_t size, const char* value) {
    if (field[0] == '\0' && value[0] != '\0') {
        snprintf(field, size, "%s", value);
    }
}

static void set_tag(struct song_meta* meta, enum tag tag, char* value) {
    switch (tag) {
        case TAG_TITLE:
            set_field(meta->title, sizeof(meta->title), value);
            break;
        case TAG_ARTIST:
            set_field(meta->artist, sizeof(meta->artist), value);
            break;
        case TAG_ALBUM:
            set_field(meta->album, sizeof(meta->album), value);
            break;
        case TAG_YEAR:
            // dates can be full like "2025-08-22", keep the year
            if (strlen(value) > 4) {
                value[4] = '\0';
            }
            set_field(meta->year, sizeof(meta->year), value);
            break;
        case TAG_TRACK: {
            // "3/12" means track 3 of 12
            char* slash = strchr(value, '/');
            if (slash) {
                *slash = '\0';
            }
            set_field(meta->track, sizeof(meta->track), value);
            break;
        }
    }
}

// remember where a cover image is. a front cover beats any other picture
static void set_cover(
    struct song_meta* meta, int* have_front,
    uint32_t type, const char* mime, off_t offset, off_t len
) {
    if (len <= 0 || (meta->art_mime[0] && (*have_front || type != FRONT_COVER))) {
        return;
    }

    // "-->" means the frame holds a link to the image, not the image
    if (strcmp(mime, "-->") == 0) {
        return;
    }

    // old taggers write just "PNG" or "JPG"
    if (strncasecmp(mime, "image/", strlen("image/")) != 0) {
        mime = strcasecmp(mime, "png") == 0 ? "image/png" : "image/jpeg";
    }

    snprintf(meta->art_mime, sizeof(meta->art_mime), "%s", mime);
    meta->art_offset = offset;
    meta->art_len = len;
    *have_front = type == FRONT_COVER;
}

static void read_id3_text_frame(FILE* f, struct song_meta* meta, const char* id, long start, long size) {
    enum tag tag;

    if (strcmp(id, "TIT2") == 0) {
        tag = TAG_TITLE;
    } else if (strcmp(id, "TPE1") == 0) {
        tag = TAG_ARTIST;
    } else if (strcmp(id, "TALB") == 0) {
        tag = TAG_ALBUM;
    } else if (strcmp(id, "TDRC") == 0 || strcmp(id, "TYER") == 0) {
        tag = TAG_YEAR;
    } else if (strcmp(id, "TRCK") == 0) {
        tag = TAG_TRACK;
    } else {
        return;
    }

    unsigned char data[TEXT_FRAME_MAX];

    if (size < 2 || size > TEXT_FRAME_MAX ||
        fseek(f, start, SEEK_SET) != 0 || fread(data, 1, size, f) != (size_t)size) {
        return;
    }

    // first byte says how the text is encoded
    char value[META_TEXT_SIZE];
    id3_text(data + 1, size - 1, data[0], value, sizeof(value));
    set_tag(meta, tag, value);
}

static void read_id3_picture(FILE* f, struct song_meta* meta, int* have_front, long start, long size) {
    unsigned char data[PICTURE_HEADER_MAX];
    size_t n = size < PICTURE_HEADER_MAX ? (size_t)size : PICTURE_HEADER_MAX;

    if (n < 4 || fseek(f, start, SEEK_SET) != 0 || fread(data, 1, n, f) != n) {
        return;
    }

    // encoding byte, then a latin1 mime type ending in NUL
    int enc = data[0];
    unsigned char* mime_end = memchr(data + 1, 0, n - 1);

    if (mime_end == NULL) {
        return;
    }

    char mime[64];
    snprintf(mime, sizeof(mime), "%.*s", (int)(mime_end - data - 1), (const char*)data + 1);

    size_t i = mime_end - data + 1;

    if (i >= n) {
        return;
    }

    uint32_t type = data[i++];

    // then a description in enc, ending in one NUL byte, or two for UTF-16
    int found_end = 0;

    if (enc == 1 || enc == 2) {
        for (; i + 1 < n; i += 2) {
            if (data[i] == 0 && data[i + 1] == 0) {
                i += 2;
                found_end = 1;
                break;
            }
        }
    } else {
        for (; i < n; i++) {
            if (data[i] == 0) {
                i++;
                found_end = 1;
                break;
            }
        }
    }

    // the image starts right after the description
    if (found_end) {
        set_cover(meta, have_front, type, mime, start + i, size - i);
    }
}

// read an ID3v2 tag from the start of f. returns where the tag ends, 0 if there isn't one
static long read_id3(FILE* f, struct song_meta* meta) {
    unsigned char header[10];

    if (fseek(f, 0, SEEK_SET) != 0 || fread(header, 1, sizeof(header), f) != sizeof(header) ||
        memcmp(header, "ID3", 3) != 0) {
        return 0;
    }

    int version = header[3];
    int flags = header[5];
    long frames_end = 10 + (long)synchsafe32(header + 6);
    long tag_end = frames_end + ((flags & 0x10) ? 10 : 0); // optional footer

    // v2.2 has different frame names, and unsynchronised tags need un-escaping
    // before they can be read. both are rare, skip them
    if ((version != 3 && version != 4) || (flags & 0x80)) {
        return tag_end;
    }

    long pos = 10;

    if (flags & 0x40) {
        unsigned char ext[4];

        if (fread(ext, 1, sizeof(ext), f) != sizeof(ext)) {
            return tag_end;
        }
        pos += version == 3 ? (long)be32(ext) + 4 : (long)synchsafe32(ext);
    }

    int have_front = 0;

    while (pos + 10 <= frames_end) {
        unsigned char frame[10];

        if (fseek(f, pos, SEEK_SET) != 0 || fread(frame, 1, sizeof(frame), f) != sizeof(frame)) {
            break;
        }

        // the rest is padding
        if (frame[0] == 0) {
            break;
        }

        long size = version == 4 ? (long)synchsafe32(frame + 4) : (long)be32(frame + 4);
        long start = pos + 10;

        if (size <= 0 || start + size > frames_end) {
            break;
        }
        pos = start + size;

        // skip compressed or encrypted frames, step over extra bytes some flags add
        unsigned char format = frame[9];

        if (version == 3) {
            if (format & 0xC0) {
                continue;
            }
            if (format & 0x20) {
                start += 1;
                size -= 1;
            }
        } else {
            if (format & 0x0E) {
                continue;
            }
            if (format & 0x40) {
                start += 1;
                size -= 1;
            }
            if (format & 0x01) {
                start += 4;
                size -= 4;
            }
        }

        if (size <= 0) {
            continue;
        }

        char id[5];
        memcpy(id, frame, 4);
        id[4] = '\0';

        if (strcmp(id, "APIC") == 0) {
            read_id3_picture(f, meta, &have_front, start, size);
        } else if (id[0] == 'T') {
            read_id3_text_frame(f, meta, id, start, size);
        }
    }

    return tag_end;
}

// true if a flac comment key like "title" matches name, ignoring case
static int key_is(const char* key, size_t key_len, const char* name) {
    return key_len == strlen(name) && strncasecmp(key, name, key_len) == 0;
}

static void read_flac_comments(FILE* f, struct song_meta* meta, long start, size_t len) {
    if (len < 8 || len > COMMENT_BLOCK_MAX) {
        return;
    }

    unsigned char* data = malloc(len);

    if (data == NULL) {
        return;
    }

    if (fseek(f, start, SEEK_SET) != 0 || fread(data, 1, len, f) != len) {
        free(data);
        return;
    }

    // vendor string, then a count of "KEY=value" comments. lengths are little endian
    size_t vendor_len = le32(data);

    if (vendor_len > len - 8) {
        free(data);
        return;
    }

    size_t pos = 4 + vendor_len;
    uint32_t count = le32(data + pos);
    pos += 4;

    for (uint32_t c = 0; c < count && pos + 4 <= len; c++) {
        size_t comment_len = le32(data + pos);
        pos += 4;

        if (comment_len > len - pos) {
            break;
        }

        const char* comment = (const char*)data + pos;
        pos += comment_len;

        const char* equals = memchr(comment, '=', comment_len);

        if (equals == NULL) {
            continue;
        }

        size_t key_len = equals - comment;
        enum tag tag;

        if (key_is(comment, key_len, "TITLE")) {
            tag = TAG_TITLE;
        } else if (key_is(comment, key_len, "ARTIST")) {
            tag = TAG_ARTIST;
        } else if (key_is(comment, key_len, "ALBUM")) {
            tag = TAG_ALBUM;
        } else if (key_is(comment, key_len, "DATE")) {
            tag = TAG_YEAR;
        } else if (key_is(comment, key_len, "TRACKNUMBER")) {
            tag = TAG_TRACK;
        } else {
            continue;
        }

        char value[META_TEXT_SIZE];
        copy_utf8(value, sizeof(value), equals + 1, comment_len - key_len - 1);
        set_tag(meta, tag, value);
    }

    free(data);
}

static void read_flac_picture(FILE* f, struct song_meta* meta, int* have_front, long start, size_t len) {
    unsigned char data[PICTURE_HEADER_MAX];
    size_t n = len < PICTURE_HEADER_MAX ? len : PICTURE_HEADER_MAX;

    if (n < 8 || fseek(f, start, SEEK_SET) != 0 || fread(data, 1, n, f) != n) {
        return;
    }

    // all big endian: type, mime, description, width/height/depth/colours, image
    uint32_t type = be32(data);
    size_t mime_len = be32(data + 4);
    size_t i = 8;

    if (mime_len > n - i) {
        return;
    }

    char mime[64];
    snprintf(mime, sizeof(mime), "%.*s", (int)mime_len, (const char*)data + i);
    i += mime_len;

    if (i + 4 > n) {
        return;
    }

    size_t desc_len = be32(data + i);
    i += 4;

    if (desc_len > n - i || i + desc_len + 20 > n) {
        return;
    }
    i += desc_len + 16;

    size_t image_len = be32(data + i);
    i += 4;

    if (image_len > len - i) {
        return;
    }

    set_cover(meta, have_front, type, mime, start + i, image_len);
}

// read flac metadata blocks if f has a flac stream at start
static void read_flac(FILE* f, struct song_meta* meta, long start) {
    unsigned char magic[4];

    if (fseek(f, start, SEEK_SET) != 0 || fread(magic, 1, sizeof(magic), f) != sizeof(magic) ||
        memcmp(magic, "fLaC", 4) != 0) {
        return;
    }

    int have_front = 0;
    long pos = start + 4;

    while (1) {
        unsigned char header[4];

        if (fseek(f, pos, SEEK_SET) != 0 || fread(header, 1, sizeof(header), f) != sizeof(header)) {
            return;
        }

        int last = header[0] & 0x80;
        int type = header[0] & 0x7F;
        size_t len = (size_t)header[1] << 16 | header[2] << 8 | header[3];
        long block = pos + 4;

        if (type == 4) {
            read_flac_comments(f, meta, block, len);
        } else if (type == 6) {
            read_flac_picture(f, meta, &have_front, block, len);
        }

        // 127 is reserved as invalid
        if (last || type == 127) {
            return;
        }
        pos = block + len;
    }
}

int read_song_meta(const char* path, struct song_meta* meta) {
    memset(meta, 0, sizeof(*meta));

    FILE* f = fopen(path, "rb");

    if (f == NULL) {
        return -1;
    }

    long id3_end = read_id3(f, meta);

    // flac files start with "fLaC", but some have an ID3 tag stuck in front
    read_flac(f, meta, id3_end);

    fclose(f);
    return 0;
}
