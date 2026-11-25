/*
KVM (Linux): the per tile bookkeeping of the capture loop (CRC and what was done with the tile in this frame), and the globals that describe the picture. No system headers, so the
code that only needs these (linux_video.c and its tests) does not need X11.

Licensed under the Apache License, Version 2.0 (same as the rest of the agent).
*/

#ifndef LINUX_TILEINFO_H_
#define LINUX_TILEINFO_H_

enum TILE_FLAGS_ENUM {
	TILE_TODO,						//The tile CRC needs to be calculated.
	TILE_SENT,						//CRC has been calculated and the tile has been sent.
	TILE_MARKED_NOT_SENT,   		//CRC has been calculated, but the tile was not sent.
	TILE_DONT_SEND		  //CRC has been calculated, tile need not be sent.
	//TILE_SKIPPED		  //CRC has been calculated, tile need not be sent, but was skipped to include a greater region
};

struct tileInfo_t {
	int crc;
	enum TILE_FLAGS_ENUM flag;
};

extern int SCREEN_WIDTH;
extern int SCREEN_HEIGHT;
extern int TILE_WIDTH;
extern int TILE_HEIGHT;
extern int TILE_WIDTH_COUNT;
extern int TILE_HEIGHT_COUNT;
extern int FRAME_RATE_TIMER;
extern struct tileInfo_t **g_tileInfo;

#endif /* LINUX_TILEINFO_H_ */
