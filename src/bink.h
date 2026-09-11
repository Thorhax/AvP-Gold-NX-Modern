#ifndef _BINK_H_
#define _BINK_H_

#ifdef __cplusplus
extern "C" {
#endif

extern BOOL BinkSys_Init(void);
extern void BinkSys_Release(void);

//--- intro/outro
extern void PlayBinkedFMV(char *filenamePtr, int volume);

//--- menu background
extern void StartMenuBackgroundBink(void);
extern int 	PlayMenuBackgroundBink(void);
extern void EndMenuBackgroundBink(void);

//---- music
extern int StartMusicBink(char* filenamePtr, BOOL looping);
extern int PlayMusicBink(int volume);
extern void EndMusicBink(void);


//---- ingame fmv
typedef unsigned int 	FMVHandle;

extern FMVHandle  	CreateBinkFMV(char* filenamePtr);
extern int			UpdateBinkFMV(FMVHandle aFmvHandle, int volume);
extern void 		CloseBinkFMV(FMVHandle aFmvHandle);
extern char*		GetBinkFMVImage(FMVHandle aFmvHandle);

#ifdef __cplusplus
}
#endif

#endif //_BINK_H_
