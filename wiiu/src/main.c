/*
 * Copyright (C) 2016 FIX94
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */
#include <string.h>
#include <malloc.h>
#include <unistd.h>
#include <stdio.h>
#include <iosuhax.h>
#include "dynamic_libs/os_functions.h"
#include "dynamic_libs/sys_functions.h"
#include "dynamic_libs/vpad_functions.h"
#include "system/memory.h"
#include "common/common.h"
#include "main.h"
#include "exploit.h"
#include "../payload/wupserver_bin.h"

static const char *vWiiVolPath = "/vol/storage_slccmpt01";
static const char *sdCardVolPath = "/vol/storage_sdcard";
static const char *vWiiAppPath = "/vol/storage_slccmpt01/title/00010002/48414341/content/00000001.app";
static const char *sdBackupPath = "/vol/storage_sdcard/wuphaxBackup.app";

//wuphax vwii executable
extern u8 boot_dol[];
extern u32 boot_dol_size;

//just to be able to call async
void someFunc(void *arg)
{
	(void)arg;
}

static int mcp_hook_fd = -1;
int MCPHookOpen()
{
	//take over mcp thread
	mcp_hook_fd = MCP_Open();
	if(mcp_hook_fd < 0)
		return -1;
	IOS_IoctlAsync(mcp_hook_fd, 0x62, (void*)0, 0, (void*)0, 0, someFunc, (void*)0);
	//let wupserver start up
	sleep(1);
	if(IOSUHAX_Open("/dev/mcp") < 0)
		return -1;
	return 0;
}

void MCPHookClose()
{
	if(mcp_hook_fd < 0)
		return;
	//close down wupserver, return control to mcp
	IOSUHAX_Close();
	//wait for mcp to return
	sleep(1);
	MCP_Close(mcp_hook_fd);
	mcp_hook_fd = -1;
}

void println(int line, const char *msg)
{
	int i;
	for(i = 0; i < 2; i++)
	{	//double-buffered font write
		OSScreenPutFontEx(0,0,line,msg);
		OSScreenPutFontEx(1,0,line,msg);
		OSScreenFlipBuffersEx(0);
		OSScreenFlipBuffersEx(1);
	}
}

int Menu_Main(void)
{
	InitOSFunctionPointers();
	InitSysFunctionPointers();
	InitVPadFunctionPointers();
	VPADInit();

	// Init screen
	OSScreenInit();
	int screen_buf0_size = OSScreenGetBufferSizeEx(0);
	int screen_buf1_size = OSScreenGetBufferSizeEx(1);
	uint8_t *screenBuffer = memalign(0x100, screen_buf0_size+screen_buf1_size);
	OSScreenSetBufferEx(0, screenBuffer);
	OSScreenSetBufferEx(1, (screenBuffer + screen_buf0_size));
	OSScreenEnableEx(0, 1);
	OSScreenEnableEx(1, 1);
	OSScreenClearBufferEx(0, 0);
	OSScreenClearBufferEx(1, 0);

	println(0,"wuphax v1.1u2 by FIX94");
	println(2,"Press A to backup your Mii Channel and inject wuphax.");
	println(3,"Press B to restore your Mii Channel from SD Card.");

	int vpadError = -1;
	VPADData vpad;
	//wait for user to decide option
	int action = 0;
	while(1)
	{
		VPADRead(0, &vpad, 1, &vpadError);

		if(vpadError == 0)
		{
			if((vpad.btns_d | vpad.btns_h) & VPAD_BUTTON_HOME)
			{
				free(screenBuffer);
				return EXIT_SUCCESS;
			}
			else if((vpad.btns_d | vpad.btns_h) & VPAD_BUTTON_A)
				break;
			else if((vpad.btns_d | vpad.btns_h) & VPAD_BUTTON_B)
			{
				action = 1;
				break;
			}
		}
		usleep(50000);
	}

	int line = 5;
	int fsaFd = -1;
	int sdMounted = 0, vWiiMounted = 0;
	int sdFd = -1, vWiiFd = -1;
	void *appBuf = NULL;

	//open up iosuhax
	int res = IOSUHAX_Open(NULL);
	if(res < 0)
		res = MCPHookOpen();
	if(res < 0)
	{
		println(line++,"Doing IOSU Exploit...");
		*(volatile unsigned int*)0xF5E70000 = wupserver_bin_len;
		memcpy((void*)0xF5E70020, &wupserver_bin, wupserver_bin_len);
		DCStoreRange((void*)0xF5E70000, wupserver_bin_len + 0x40);
		IOSUExploit();
		//done with iosu exploit, take over mcp
		if(MCPHookOpen() < 0)
		{
			println(line++,"MCP hook could not be opened!");
			goto prgEnd;
		}
		println(line++,"Done!");
	}

	//mount with full permissions
	fsaFd = IOSUHAX_FSA_Open();
	if(fsaFd < 0)
	{
		println(line++,"FSA could not be opened!");
		goto prgEnd;
	}
	int ret = IOSUHAX_FSA_Mount(fsaFd, "/dev/sdcard01", sdCardVolPath, 2, (void*)0, 0);
	if(ret < 0)
	{
		println(line++,"Failed to mount SD!");
		goto prgEnd;
	}
	else
		sdMounted = 1;
	ret = IOSUHAX_FSA_Mount(fsaFd, "/dev/slccmpt01", vWiiVolPath, 2, (void*)0, 0);
	if(ret < 0)
	{
		println(line++,"Failed to mount vWii NAND!");
		goto prgEnd;
	}
	else
		vWiiMounted = 1;

	if(action == 0) //backup and inject
	{
		//read in currently written app
		ret = IOSUHAX_FSA_OpenFile(fsaFd, vWiiAppPath, "rb", &vWiiFd);
		if(ret < 0)
		{
			println(line++,"Failed to open mii channel app!");
			goto prgEnd;
		}
		fileStat_s stats;
		ret = IOSUHAX_FSA_StatFile(fsaFd, vWiiFd, &stats);
		if(ret < 0)
		{
			println(line++,"Failed to stat app file!");
			goto prgEnd;
		}
		char *appBuf = malloc(stats.size);
		size_t done = 0;
		while(done < stats.size)
		{
			size_t read_size = stats.size - done;
			int result = IOSUHAX_FSA_ReadFile(fsaFd, appBuf + done, 0x01, read_size, vWiiFd, 0);
			if(result <= 0)
			{
				println(line++,"Failed to read app file!");
				goto prgEnd;
			}
			else
				done += result;
		}
		IOSUHAX_FSA_CloseFile(fsaFd, vWiiFd);
		vWiiFd = -1;
		//check if backup already exists
		ret = IOSUHAX_FSA_OpenFile(fsaFd, sdBackupPath, "rb", &sdFd);
		if(ret < 0)
		{
			//no backup there yet, time to write it to sd
			ret = IOSUHAX_FSA_OpenFile(fsaFd, sdBackupPath, "wb", &sdFd);
			if(ret < 0)
			{
				println(line++,"Failed to open backup file!");
				goto prgEnd;
			}
			done = 0;
			while(done < stats.size)
			{
				size_t write_size = stats.size - done;
				int result = IOSUHAX_FSA_WriteFile(fsaFd, appBuf + done, 0x01, write_size, sdFd, 0);
				if(result <= 0)
				{
					println(line++,"Failed to write backup file!");
					goto prgEnd;
				}
				else
					done += result;
			}
			IOSUHAX_FSA_CloseFile(fsaFd, sdFd);
			sdFd = -1;
		}
		else
		{
			//backup already exists
			IOSUHAX_FSA_CloseFile(fsaFd, sdFd);
			sdFd = -1;
		}
		//inject custom boot.dol
		memcpy(appBuf, boot_dol, boot_dol_size);
		DCStoreRange(appBuf, boot_dol_size);
		ret = IOSUHAX_FSA_OpenFile(fsaFd, vWiiAppPath, "wb", &vWiiFd);
		if(ret < 0)
		{
			println(line++,"Failed to open mii channel app!");
			goto prgEnd;
		}
		done = 0;
		while(done < stats.size)
		{
			size_t write_size = stats.size - done;
			int result = IOSUHAX_FSA_WriteFile(fsaFd, appBuf + done, 0x01, write_size, vWiiFd, 0);
			if(result <= 0)
			{
				println(line++,"Failed to write injected app!");
				goto prgEnd;
			}
			else
				done += result;
		}
		println(line++,"Successfully injected wuphax!");
		//done writing injected app
		IOSUHAX_FSA_CloseFile(fsaFd, vWiiFd);
		vWiiFd = -1;
	}
	else //restore channel
	{
		//open up sd backup path
		ret = IOSUHAX_FSA_OpenFile(fsaFd, sdBackupPath, "rb", &sdFd);
		if(ret < 0)
		{
			println(line++,"Backup file not found!");
			goto prgEnd;
		}
		fileStat_s stats;
		ret = IOSUHAX_FSA_StatFile(fsaFd, sdFd, &stats);
		if(ret < 0)
		{
			println(line++,"Failed to stat backup file!");
			goto prgEnd;
		}
		appBuf = malloc(stats.size);
		size_t done = 0;
		while(done < stats.size)
		{
			size_t read_size = stats.size - done;
			int result = IOSUHAX_FSA_ReadFile(fsaFd, appBuf + done, 0x01, read_size, sdFd, 0);
			if(result <= 0)
			{
				println(line++,"Failed to read backup file!");
				goto prgEnd;
			}
			else
				done += result;
		}
		IOSUHAX_FSA_CloseFile(fsaFd, sdFd);
		sdFd = -1;
		//open up vwii app path to restore
		ret = IOSUHAX_FSA_OpenFile(fsaFd, vWiiAppPath, "wb", &vWiiFd);
		if(ret < 0)
		{
			println(line++,"Failed to open mii channel app!");
			goto prgEnd;
		}
		done = 0;
		while(done < stats.size)
		{
			size_t write_size = stats.size - done;
			int result = IOSUHAX_FSA_WriteFile(fsaFd, appBuf + done, 0x01, write_size, vWiiFd, 0);
			if(result <= 0)
			{
				println(line++,"Failed to write back app file!");
				goto prgEnd;
			}
			else
				done += result;
		}
		println(line++,"Mii Channel restored!");
		//restored original channel app
		IOSUHAX_FSA_CloseFile(fsaFd, vWiiFd);
		vWiiFd = -1;
	}

prgEnd:
	//used to deal with app
	if(appBuf)
		free(appBuf);
	//close down everything fsa related
	if(fsaFd >= 0)
	{
		if(sdFd >= 0)
			IOSUHAX_FSA_CloseFile(fsaFd, sdFd);
		if(vWiiFd >= 0)
			IOSUHAX_FSA_CloseFile(fsaFd, vWiiFd);
		if(sdMounted)
			IOSUHAX_FSA_Unmount(fsaFd, sdCardVolPath, 2);
		if(vWiiMounted)
			IOSUHAX_FSA_Unmount(fsaFd, vWiiVolPath, 2);
		IOSUHAX_FSA_Close(fsaFd);
	}
	//close out iosuhax
	if(mcp_hook_fd >= 0)
		MCPHookClose();
	else
		IOSUHAX_Close();
	sleep(5);
	//will do IOSU reboot
	OSForceFullRelaunch();
	SYSLaunchMenu();
	OSScreenEnableEx(0, 0);
	OSScreenEnableEx(1, 0);
	free(screenBuffer);
	return EXIT_RELAUNCH_ON_LOAD;
}
