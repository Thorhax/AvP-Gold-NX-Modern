#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fixer.h"
#include "bink.h"

BOOL BinkSys_Init(void)
{
	return TRUE;
}

void BinkSys_Release(void)
{
}

void PlayBinkedFMV(char *filenamePtr, int volume)
{
	(void)filenamePtr;
	(void)volume;
}

void StartMenuBackgroundBink(void)
{
}

int PlayMenuBackgroundBink(void)
{
	return 0;
}

void EndMenuBackgroundBink(void)
{
}

int StartMusicBink(char* filenamePtr, BOOL looping)
{
	(void)filenamePtr;
	(void)looping;
	return 0;
}

int PlayMusicBink(int volume)
{
	(void)volume;
	return 0;
}

void EndMusicBink(void)
{
}

FMVHandle CreateBinkFMV(char* filenamePtr)
{
	(void)filenamePtr;
	return 0;
}

int UpdateBinkFMV(FMVHandle aFmvHandle, int volume)
{
	(void)aFmvHandle;
	(void)volume;
	return 0;
}

void CloseBinkFMV(FMVHandle aFmvHandle)
{
	(void)aFmvHandle;
}

char* GetBinkFMVImage(FMVHandle aFmvHandle)
{
	(void)aFmvHandle;
	return NULL;
}
