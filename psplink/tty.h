/*
 * PSPLINK
 * -----------------------------------------------------------------------
 * Licensed under the BSD license, see LICENSE in PSPLINK root for details.
 *
 * tty.h - PSPLINK kernel module tty code
 *
 * Copyright (c) 2005 James F <tyranid@gmail.com>
 *
 * $HeadURL$
 * $Id$
 */

void ttySetUsbHandler(PspDebugPrintHandler usbShellHandler, PspDebugPrintHandler usbStdoutHandler, PspDebugPrintHandler usbStderrHandler, PspDebugInputHandler usbStdinHandler);
void ttyInit(void);
