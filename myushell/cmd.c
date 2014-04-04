/*
 * cmd.c - command shell
 *
 */

#include <pspkernel.h>
#include <pspsdk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "usbshell.h"


int print_modinfo(SceUID uid, int verbose)
{
	SceKernelModuleInfo info;
	int retv;
	int i;

	memset(&info, 0, sizeof(info));
	info.size = sizeof(info);
	retv = sceKernelQueryModuleInfo(uid, &info);
    if(retv<0){
		printk("Error querying module %08x\n", retv);
		return retv;
	}

	printk("UID: 0x%08X Attr: %04X - Name: %s\n", uid, info.attribute, info.name);

	if(verbose){
		printk("Entry: 0x%08X - GP: 0x%08X - TextAddr: 0x%08X\n", info.entry_addr,
					info.gp_value, info.text_addr);
		printk("TextSize: 0x%08X - DataSize: 0x%08X BssSize: 0x%08X\n", info.text_size,
					info.data_size, info.bss_size);
		for(i=0; (i<info.nsegment) && (i<4); i++){
			printk("Segment %d: Addr 0x%08X - Size 0x%08X\n", i,
					(unsigned int) info.segmentaddr[i], (unsigned int) info.segmentsize[i]);
		}
		printk("\n");
	}

	return 0;
}

int modlist_cmd(void)
{
	SceUID ids[100];
	int i, retv, count;

    memset(ids, 0, 100 * sizeof(SceUID));
    retv = sceKernelGetModuleIdList(ids, 100*sizeof(SceUID), &count);
    if(retv >= 0){
        printk("<Module List (%d modules)>\n", count);
        for(i=0; i<count; i++){
            print_modinfo(ids[i], 0);
        }
    }

	return 0;
}


int parse_cmd(char *cmdbuf)
{
	if(strncmp(cmdbuf, "ml", 2)==0){
		modlist_cmd();
	}


	return 0;
}

