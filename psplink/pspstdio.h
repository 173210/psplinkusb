/*
 * PSPLINK
 * -----------------------------------------------------------------------
 * Licensed under the BSD license, see LICENSE in PSPLINK root for details.
 *
 * stdio.h - Module library code for psplink
 *
 * Copyright (c) 2006 James F
 *
 * $HeadURL: svn://tyranid@svn.pspdev.org/psp/trunk/psplink/psplink/stdio.h $
 * $Id: stdio.h 1910 2006-05-14 18:39:19Z tyranid $
 */
#ifndef __PSP_STDIO_H__
#define __PSP_STDIO_H__

#define SHELL_FILENO 3
extern int g_shellfd;

int stdioTtyInit(void);
int stdioInstallStdinHandler(PspDebugInputHandler handler);
int stdioInstallStdoutHandler(PspDebugPrintHandler handler);
int stdioInstallStderrHandler(PspDebugPrintHandler handler);
int stdioInstallShellHandler(PspDebugPrintHandler handler);

#endif
