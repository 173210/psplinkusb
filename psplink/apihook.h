/*
 * PSPLINK
 * -----------------------------------------------------------------------
 * Licensed under the BSD license, see LICENSE in PSPLINK root for details.
 *
 * apihook.h - User mode API Hooking for psplink
 *
 * Copyright (c) 2005 James F <tyranid@gmail.com>
 *
 * $HeadURL$
 * $Id$
 */
#ifndef __APIHOOK_H__
#define __APIHOOK_H__

unsigned int apiHookByName(SceUID uid, const char *library, const char *name, void *func);
unsigned int apiHookByNid(SceUID uid, const char *library, unsigned int nid, void *func);
int apiHookGenericByName(SceUID uid, const char *library, const char *name, char ret, const char *format, int sleep);
int apiHookGenericByNid(SceUID uid, const char *library, unsigned int nid, char ret, const char *format, int sleep);
void apiHookGenericDelete(int id);
void apiHookGenericPrint(void);

#endif
