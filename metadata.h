#ifndef METADATA_H
#define METADATA_H

#include <sys/types.h>

#define META_TEXT_SIZE 256

struct song_meta {
    char title[META_TEXT_SIZE];
    char artist[META_TEXT_SIZE];
    char album[META_TEXT_SIZE];
    char year[16];
    char track[16];
    char art_mime[64];  // empty if the song has no cover
    off_t art_offset;   // where the cover image bytes start in the song file
    off_t art_len;
};

// read tags and where the cover is from an mp3 (ID3v2) or flac file.
// fields it can't find are left empty. -1 if the file can't be opened
int read_song_meta(const char* path, struct song_meta* meta);

#endif
