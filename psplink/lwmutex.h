/*
 * PSPLINK Modfied Version
 * -----------------------------------------------------------------------
 * Licensed under the BSD license, see LICENSE in PSPLINK root for details.
 *
 * lwmutex.h - LwMutex Header
 *
 * Copyright (c) 2013 173210 <root.3.173210@live.com>
 */

typedef struct {
	int count;
	SceUID thread;
	int attr;
	int numWaitThreads;
	SceUID uid;
	int pad[3];
} SceLwMutexWorkarea;

typedef struct {
	SceSize size;
	char name[32];
	SceUInt attr;
	SceUID uid;
	SceLwMutexWorkarea *workarea;
	int initCount;
	int currentCount;
	SceUID lockThread;
	int numWaitThreads;
} SceKernelLwMutexInfo;

int sceKernelReferLwMutexStatusByID(SceUID mutexID, SceKernelLwMutexInfo *status);
