/*
 * PSPLINK
 * -----------------------------------------------------------------------
 * Licensed under the BSD license, see LICENSE in PSPLINK root for details.
 *
 * main.c - PSPLINK USB Shell main code
 *
 * Copyright (c) 2006 James F <tyranid@gmail.com>
 *
 * $HeadURL: svn://svn.pspdev.org/psp/trunk/psplinkusb/usbshell/main.c $
 * $Id: main.c 2034 2006-10-18 17:02:37Z tyranid $
 */
#include <pspkernel.h>
#include <pspdebug.h>
#include <pspkdebug.h>
#include <pspsdk.h>
#include <pspusb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <usbhostfs.h>
#include <usbasync.h>

#include "systemctrl.h"
#include "usbshell.h"

PSP_MODULE_INFO("USBShell", PSP_MODULE_KERNEL, 1, 1);

#define HOSTFSDRIVER_NAME "USBHostFSDriver"
#define HOSTFSDRIVER_PID  (0x1C9)

struct AsyncEndpoint g_endp;
struct AsyncEndpoint g_stdin;

static int usbStdoutPrint(const char *data, int size)
{
	usbAsyncWrite(ASYNC_STDOUT, data, size);

	return size;
}

static int usbStderrPrint(const char *data, int size)
{
	usbAsyncWrite(ASYNC_STDERR, data, size);

	return size;
}


STMOD_HANDLER previous;

int on_module_start(SceModule2 *mod)
{
	int *fontopen_table;
	int i;

	if(strcmp(mod->modname, "scePaf_Module")==0){
		printk("Patch scePaf_Module!\n");
		print_modinfo(mod->modid, 1);
	
		fontopen_table = (int*)(mod->text_addr+0x001a03d0);
		for(i=0; i<10; i++){
			printk("table %2d: %08x\n", i, fontopen_table[i]);
		}

		int pafFontOpen = fontopen_table[0];
		int *pafFontOpenGb = (int*)fontopen_table[4];

		/* j pafFontOpen */
		pafFontOpenGb[0] = 0x08000000|(pafFontOpen>>2);
		/* move $a1, 0 */
		pafFontOpenGb[1] = 0x00002821;

	}

	if(previous)
		return previous(mod);

	return 0;
}






static int main_thread(SceSize args, void *argp)
{
	char cmdbuf[128];
	int cpos;
	int retv;

	cpos = 0;
	while(1){
		if(usbAsyncRead(ASYNC_STDOUT, (unsigned char*)&cmdbuf[cpos], 1)<1){
			sceKernelDelayThread(250000);
			continue;
		}
		if(cmdbuf[cpos]=='\n'){
			cmdbuf[cpos] = 0;
			retv = parse_cmd(cmdbuf);
			cpos = 0;
		}else{
			if(cpos<127)
				cpos++;
		}
	}

	sceKernelExitDeleteThread(0);
	return 0;
}

/* Entry point */
int module_start(SceSize args, void *argp)
{
	int thid;
	int retv;

	retv = sceUsbStart(PSP_USBBUS_DRIVERNAME, 0, 0);
	if(retv){
		return 0;
	}
	retv = sceUsbStart(HOSTFSDRIVER_NAME, 0, 0);
	if(retv){
		return 0;
	}
	retv = sceUsbActivate(HOSTFSDRIVER_PID);

	usbAsyncRegister(ASYNC_SHELL, &g_endp);
	usbAsyncRegister(ASYNC_STDOUT, &g_stdin);
	usbWaitForConnect();

	retv = stdioTtyInit();
	stdioInstallStdoutHandler(usbStdoutPrint);
	stdioInstallStderrHandler(usbStderrPrint);

	printk("Usbshell Start!\n");

	previous = sctrlHENSetStartModuleHandler(on_module_start);

	/* Create a high priority thread */
	thid = sceKernelCreateThread("USBShell", main_thread, 12, 0x2000, 0, NULL);
	if(thid >= 0)
	{
		sceKernelStartThread(thid, args, argp);
	}
	return 0;
}

/* Module stop entry */
int module_stop(SceSize args, void *argp)
{
	return 0;
}

