#ifndef MANAGE_H
#define MANAGE_H

// the commands that look after sonded rather than serve music:
//   sonded update            download and install the latest release
//   sonded ts <folder>       serve folder in the background and share it on the tailnet
//   sonded ts off            undo sonded ts
//   sonded version
// returns the exit code, or -1 if argv[1] isn't one of these (so it's a folder)
int manage_command(int argc, char** argv);

// on Windows, remove the sonded.old.exe an update left behind. nothing elsewhere
void remove_old_binary(void);

#endif
