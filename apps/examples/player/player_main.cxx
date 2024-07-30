/****************************************************************************
 *
 * Copyright 2024 Samsung Electronics All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
 * either express or implied. See the License for the specific
 * language governing permissions and limitations under the License.
 *
 ****************************************************************************/

//***************************************************************************
// Included Files
//***************************************************************************

#include <tinyara/config.h>

#include <cstdio>
#include <debug.h>
#include <media/MediaPlayer.h>
#include <media/MediaPlayerObserverInterface.h>
#include <media/FileInputDataSource.h>

#include <iostream>
#include <memory>

using namespace std;
using namespace media;
using namespace media::stream;

#define AUDIO_SAMPLE_RATE 24000
#define NUMBER_OF_CHANNELS 1
#define FILE_PATH "/mnt/audio.pcm"

static media::MediaPlayer mp;
static int play_count = 0;
static void playData(void);

class _Observer : public media::MediaPlayerObserverInterface, public std::enable_shared_from_this<_Observer>
{
	void onPlaybackStarted(media::MediaPlayer &mediaPlayer) override
	{
		printf("[PLAYER_TEST] ##################################\n");
		printf("[PLAYER_TEST] ####    onPlaybackStarted     ####\n");
		printf("[PLAYER_TEST] ##################################\n");
		play_count++;
	}
	void onPlaybackFinished(media::MediaPlayer &mediaPlayer) override
	{
		printf("[PLAYER_TEST] ##################################\n");
		printf("[PLAYER_TEST] ####    onPlaybackFinished    ####\n");
		printf("[PLAYER_TEST] ##################################\n");

		mp.unprepare();
		mp.destroy();

	}
	void onPlaybackError(media::MediaPlayer &mediaPlayer, media::player_error_t error) override
	{
		printf("[PLAYER_TEST] ##################################\n");
		printf("[PLAYER_TEST] ####      onPlaybackError     ####\n");
		printf("[PLAYER_TEST] ##################################\n");
	}
	void onStartError(media::MediaPlayer &mediaPlayer, media::player_error_t error) override
	{
	}
	void onStopError(media::MediaPlayer &mediaPlayer, media::player_error_t error) override
	{
	}
	void onPauseError(media::MediaPlayer &mediaPlayer, media::player_error_t error) override
	{
	}
	void onPlaybackPaused(media::MediaPlayer &mediaPlayer) override
	{
	}
};

void playData(void)
{
	mp.create();
	auto source = std::move(unique_ptr<media::stream::FileInputDataSource>(new media::stream::FileInputDataSource(FILE_PATH)));
	source->setSampleRate(AUDIO_SAMPLE_RATE);
	source->setChannels(NUMBER_OF_CHANNELS);
	source->setPcmFormat(media::AUDIO_FORMAT_TYPE_S16_LE);
	mp.setObserver(std::make_shared<_Observer>());
	mp.setDataSource(std::move(source));
	mp.prepare();
	mp.setVolume(9);
	mp.start();
}

extern "C" {
int player_main(int argc, char *argv[])
{
	printf("player_main Entry\n");
	while(play_count<100) {
		//cout<<"play_count :"<<play_count<<endl;
		playData();
	}
	return 0;
}
}