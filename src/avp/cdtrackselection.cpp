extern "C"
{
#include "3dc.h"
#include "ourasert.h"
#include "psndplat.h"
#include "dxlog.h"
#include "cd_player.h"
#include "avp_menus.h"
#include "gamedef.h"

#include "avp_envinfo.h"
};

#include "list_tem.hpp"

//lists of tracks for each level
List<int> LevelCDTracks[AVP_ENVIRONMENT_END_OF_LIST];

//lists of tracks for each species in multiplayer games
List<int> MultiplayerCDTracks[3];

static int LastTrackChosen=-1;

extern "C"
{

void EmptyCDTrackList()
{
	for(int i=0;i<AVP_ENVIRONMENT_END_OF_LIST;i++)
	{
		while(LevelCDTracks[i].size()) LevelCDTracks[i].delete_first_entry();
	}

	for(int i=0;i<3;i++)
	{
		while(MultiplayerCDTracks[i].size()) MultiplayerCDTracks[i].delete_first_entry();
	}
}

#define CDTrackFileName "cd tracks.txt"


static void ExtractTracksForLevel(char* & buffer,List<int> & track_list)
{
	//search for a line starting with a #
	while(*buffer)
	{
		if(*buffer=='#') break;
		//search for next line
		while(*buffer)
		{
			if(*buffer=='\n')
			{
				buffer++;
				if(*buffer=='\r') buffer++;
				break;
			}
			buffer++;
		}

	}
	
	while(*buffer)
	{
		//search for a track number or comment
		if(*buffer==';')
		{
			//comment , so no further info on this line
			break;
		}
		else if(*buffer=='\n' || *buffer=='\r')
		{
			//reached end of line
			break;
		}
		else if(*buffer>='0' && *buffer<='9')
		{
			int track=-1;
			//find a number , add it to the list
			sscanf(buffer,"%d",&track);

			if(track>=0)
			{
				track_list.add_entry(track);
			}

			//skip to the next non numerical character
			while(*buffer>='0' && *buffer<='9') buffer++;
		}
		else
		{
			buffer++;
		}
	}

	//go to the next line
	while(*buffer)
	{
		if(*buffer=='\n')
		{
			buffer++;
			if(*buffer=='\r') buffer++;
			break;
		}
		buffer++;
	}
	
}

static void SetDefaultCDTrackList()
{
	// Multiplayer / default tracks
	for(int t=1; t<=5; t++) MultiplayerCDTracks[0].add_entry(t); // Marine
	for(int t=6; t<=10; t++) MultiplayerCDTracks[1].add_entry(t); // Predator
	for(int t=11; t<=15; t++) MultiplayerCDTracks[2].add_entry(t); // Alien

	// Marine levels
	int m_tracks[5][5] = {
		{1,2,3,4,5},
		{2,3,4,5,1},
		{3,4,5,1,2},
		{4,5,1,2,3},
		{5,1,2,3,4}
	};
	for(int i=0; i<5; i++) {
		for(int t=0; t<5; t++) LevelCDTracks[AVP_ENVIRONMENT_DERELICT + i].add_entry(m_tracks[i][t]);
	}

	// Predator levels
	int p_tracks[5][5] = {
		{6,7,8,9,10},
		{7,8,9,10,6},
		{8,9,10,6,7},
		{9,10,6,7,8},
		{10,6,7,8,9}
	};
	for(int i=0; i<5; i++) {
		for(int t=0; t<5; t++) LevelCDTracks[AVP_ENVIRONMENT_WATERFALL + i].add_entry(p_tracks[i][t]);
	}

	// Alien levels
	int a_tracks[5][5] = {
		{11,12,13,14,15},
		{12,13,14,15,11},
		{13,14,15,11,12},
		{14,15,11,12,13},
		{15,11,12,13,14}
	};
	for(int i=0; i<5; i++) {
		for(int t=0; t<5; t++) LevelCDTracks[AVP_ENVIRONMENT_FERARCO + i].add_entry(a_tracks[i][t]);
	}
}

void LoadCDTrackList()
{
	//clear out the old list first
	EmptyCDTrackList();

	FILE *file = OpenGameFile(CDTrackFileName, FILEMODE_READONLY, FILETYPE_OPTIONAL);
	
	if(file==NULL)
	{
		LOGDXFMT(("Failed to open %s, using defaults",CDTrackFileName));
		SetDefaultCDTrackList();
		return;
	}

	char* buffer;
	int file_size;

	fseek(file, 0, SEEK_END);
	file_size = ftell(file);
	rewind(file);
	
	//copy the file contents into a buffer
	buffer=new char[file_size+1];
	fread(buffer, 1, file_size, file);
	buffer[file_size]='\0';
	fclose(file);
	
	char* bufferptr=buffer;


	//first extract the multiplayer tracks
	for(int i=0;i<3;i++)
	{
		ExtractTracksForLevel(bufferptr,MultiplayerCDTracks[i]);
	}
	
	//now the level tracks
	for(int i=0 ;i<AVP_ENVIRONMENT_END_OF_LIST;i++)
	{
		ExtractTracksForLevel(bufferptr,LevelCDTracks[i]);
	}
	
	delete [] buffer;

	if (MultiplayerCDTracks[0].size() == 0) {
		SetDefaultCDTrackList();
	}
}

static unsigned int TrackSelectCounter=0;

static BOOL PickCDTrack(List<int>& track_list)
{
	//make sure we have some tracks in the list
	if(!track_list.size()) return FALSE;

	//pick the next track in the list
	unsigned int index=TrackSelectCounter % track_list.size();

	TrackSelectCounter++;

	//play it
	CDDA_Stop();
	CDDA_Play(track_list[index]);

	LastTrackChosen = track_list[index];
	return TRUE;	
}


void CheckCDAndChooseTrackIfNeeded()
{
	static enum playertypes lastPlayerType;
	
	//are we bothering with cd tracks
	if(!CDDA_IsOn()) return;
	//is our current track still playing
	if(CDDA_IsPlaying())
	{
		//if in a multiplayer game see if we have changed character type
		if(AvP.Network == I_No_Network || AvP.PlayerType==lastPlayerType) 
			return;
		
		//have changed character type , is the current track in the list for this character type
		if(MultiplayerCDTracks[AvP.PlayerType].contains(LastTrackChosen)) 
			return;

		//Lets choose a new track then
	}

		
	if(AvP.Network == I_No_Network)
	{
		int level=NumberForCurrentLevel();
		if(level>=0 && level<AVP_ENVIRONMENT_END_OF_LIST)
		{
			//pick track based on level
			if(PickCDTrack(LevelCDTracks[level]))
			{
				return;
			}
			
		}
	}
	
	
	//multiplayer (or their weren't ant level specific tracks)
	lastPlayerType=AvP.PlayerType;
	PickCDTrack(MultiplayerCDTracks[AvP.PlayerType]);
	
	 	
}

void ResetCDPlayForLevel()
{
	//check the number of tracks available while we're at it
	CDDA_CheckNumberOfTracks();

	TrackSelectCounter=0;
	CDDA_Stop();
}

};
