/*
 * UTZ - A beatmatching drum machine
 *
 * Copyright 2016 Hans Holmberg
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <alsa/asoundlib.h>
#include <signal.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <time.h>
#include <pthread.h>

#define BPM_MEASUREPOINTS  16

int stop=0;
void sighandler(int ignored)
{
        stop=1;
}

int print_info(snd_rawmidi_t *MIDI_in)
{
	int ret = 0;
	snd_rawmidi_info_t *MIDI_info;

	ret = snd_rawmidi_info_malloc(&MIDI_info);
	if (ret) 
		return ret;
		
	ret = snd_rawmidi_info(MIDI_ingit , MIDI_info);
	if (ret)
		return ret;

	printf("UTZ! Using input from driver: %s, card: %s \n",
		snd_rawmidi_info_get_id(MIDI_info),		
		snd_rawmidi_info_get_name(MIDI_info));

	snd_rawmidi_info_free(MIDI_info);
	return 0;
}

#define MIDI_STATUS_MASK 0xF0

enum MIDI_STATUS {
	NOTE_OFF	= 	8,
	NOTE_ON,
	POLYPHONIC_KEY_PRESSURE,
	CONTROL_CHANGE,
	PROGRAM_CHANGE,
	CHANNEL_PRESSURE,
	PITCH_BEND_CHANGE,
	SYSTEM_EXCLUSIVE
	
};

/* See https://www.midi.org/specifications/item/table-1-summary-of-midi-message */
int getMIDImessage(snd_rawmidi_t *MIDI_in){
	unsigned char buffer[3];
	int ret = -1;
	int status;

	ret = snd_rawmidi_read(MIDI_in, buffer, 1);

	if (ret < 0) {
		printf("Error reading midi input\n");
		return ret;
	}
	
	status = (buffer[0] & 0xF0) >> 4;
	
	switch (status) {
		case NOTE_OFF:
		case NOTE_ON:
			/* read note number & velocity, ignore for now */
			ret = snd_rawmidi_read(MIDI_in, buffer, 2);
			if (ret < 0)
				return ret;			
			return status;
	
		case POLYPHONIC_KEY_PRESSURE:		
		case CONTROL_CHANGE:
			ret = snd_rawmidi_read(MIDI_in, buffer, 2);
			if (ret < 0)
				return ret;			
			return status;

		case PROGRAM_CHANGE:
		case CHANNEL_PRESSURE:
		case PITCH_BEND_CHANGE:
			ret = snd_rawmidi_read(MIDI_in, buffer, 1);
			if (ret < 0)
				return ret;			
			return status;

		case SYSTEM_EXCLUSIVE: /* we do not support system exclusive messages */
		default:
			ret =  -1;
	}
	return ret;
}

void stupidsort(long *array, long elements) {
	int i,j;
	for(i = 0; i < (elements-1); i++ ) {
		for(j = 0; j < (elements - i - 1); j++)
		{
			if (array[j] < array[j+1]) {
				long temp = array[j];
				array[j] = array[j+1];
				array[j+1] = temp;
			}
		}
	}
}


char *audio_output = "default";
char *midi_input = "hw:1,0,0";
char *wav_filename = NULL;

void print_usage() {
	fprintf(stderr, "usage: utz [option(s)] samplefile(.wav)\n");
	fprintf(stderr, "supported options [defaults in brackets]:\n");
	fprintf(stderr, "-l, --list-devices      list available audio output devices\n");
	fprintf(stderr, "-o, --output            set output audio device [%s]\n", audio_output);
	fprintf(stderr, "-i, --input             set MIDI input device(tip: use amidi -l to list available devs)\n");
}

void list_devices() {
	char **hints;
	char ** n;
	int err = snd_device_name_hint(-1, "pcm", (void***)&hints);
	if (err != 0)
		return;
	n = hints;
	printf("Available PCM devices: \n");
	while (*n != NULL) {
		char *name = snd_device_name_get_hint(*n, "NAME");
		if (name != NULL && 0 != strcmp("null", name)) {
			printf("    %s \n", name);
			free(name);
		}
		n++;
	}
	snd_device_name_free_hint((void**)hints);
}

int parse_cmdargs(int argv, char *argc[]) {
	int i=1;
	/* parse otions */
	while (i < argv) {
		int option_identified = 0;
		if (strcmp(argc[i], "-l") == 0 ||
		    strcmp(argc[i], "--list-devices") == 0) {
			list_devices();
			return -1;
		}

		if (strcmp(argc[i], "-o") == 0 ||
		    strcmp(argc[i], "--output") == 0) {
			if (i < argv-1) {
				i++;
				audio_output = argc[i++];
			} else {
				fprintf(stderr, "output device not specified\n");
				return -1;
			}
			option_identified = 1;
		}

		if (strcmp(argc[i], "-i") == 0 ||
		    strcmp(argc[i], "--input") == 0) {
			if (i < argv-1) {
				i++;
				midi_input = argc[i++];
			} else {
				fprintf(stderr, "MIDI input device not specified\n");
				return -1;
			}
			option_identified = 1;
		}

		if (!option_identified) {
			if (i == argv-1) {
				/* last option -> wav sample name, we're done here */
				wav_filename = argc[i];
				return 0;
			} else {
				fprintf(stderr, "unknown option: %s\n", argc[i]);
				return -1;
			}
		}
	}

	fprintf(stderr, "No wav specified!\n");
	print_usage();
	return -1;
}

snd_pcm_t *init_sound(char *pcm_name, int rate, int channels)
{
	snd_pcm_t *pcm_handle = NULL;
	int ret;

	ret = snd_pcm_open(&pcm_handle, pcm_name,
				SND_PCM_STREAM_PLAYBACK, 0);
	if (ret < 0) {
		fprintf(stderr, "Can't open \"%s\" PCM device. %s\n",
					pcm_name, snd_strerror(ret));
		return NULL;
	}

	ret = snd_pcm_set_params(pcm_handle,
				     SND_PCM_FORMAT_S16_LE,
				     SND_PCM_ACCESS_RW_INTERLEAVED,
				     channels,
				     rate,
				     1,  /*soft resample allowed*/
                                     5 /*20ms latency*/);
	if (ret < 0) {

		fprintf(stderr, "Can't set PCM parameters: %s\n",
					snd_strerror(ret));
		return NULL;
	}

	return pcm_handle;
}

int play_sound(snd_pcm_t *pcm_handle, char *wav_filename)
{
	snd_pcm_uframes_t buffer_size;
	snd_pcm_uframes_t period_size;
	int ret;
	int wav_fd;
	char *buffer;
	int bytesread = 0;
	int readbuffer_size;

	ret = snd_pcm_get_params(pcm_handle, &buffer_size, &period_size);
	if (ret < 0) {
		fprintf(stderr, "Could not get pcm parameters: %s \n",
			snd_strerror(ret));
		return -1;
	}
	/*printf("Buffer size: %ld, Period size: %ld \n", buffer_size, period_size);*/

	/* TODO: add real wav file reading */
	wav_fd = open(wav_filename, O_RDONLY);
	if(-1 == wav_fd) {
		fprintf(stderr, "could not open wav file\n");
		return -1;
	}
	lseek(wav_fd, 44, SEEK_SET);

	readbuffer_size = period_size*2*2;
	buffer = malloc(readbuffer_size);
	if(NULL == buffer) {
		fprintf(stderr, "Could not allocate play buffer\n");
		return -1;
	}

	while( (bytesread = read(wav_fd, buffer, readbuffer_size)) != 0 ) {

		/* zero the last bytes of the buffer if we're at the end of the file */
		if (bytesread < readbuffer_size)
			memset(&buffer[bytesread], readbuffer_size-bytesread, 0);

		ret = snd_pcm_writei(pcm_handle, buffer, period_size);
		if (ret == -EPIPE) {
			/*fprintf(stderr, "XRUN\n");*/
			snd_pcm_prepare(pcm_handle);
		}
	}

	close(wav_fd);
	free(buffer);
	return 0;
}

pthread_mutex_t play_time_lock;
long beat_period_ms=0;

struct timespec play_time;

void* playsound_thread(void *arg) {
	snd_pcm_t *pcm_handle = arg;
	struct timespec current_time;

	while(!stop) {
		pthread_mutex_lock(&play_time_lock);
		clock_gettime(CLOCK_REALTIME, &current_time);
		if( play_time.tv_sec != 0 &&
		     (current_time.tv_sec > play_time.tv_sec ||
	              (current_time.tv_sec == play_time.tv_sec &&
                       current_time.tv_nsec > play_time.tv_nsec))) {

			if(beat_period_ms == 0) {
				play_time.tv_sec = 0;
			} else {
				play_time.tv_sec = (current_time.tv_sec + beat_period_ms/1000);
				play_time.tv_nsec = (current_time.tv_nsec + beat_period_ms*1000*1000) % (1000000000);
				play_time.tv_sec += (current_time.tv_nsec + beat_period_ms*1000*1000) / (1000000000);
			}

			pthread_mutex_unlock(&play_time_lock);
			play_sound(pcm_handle, wav_filename);
		} else
			pthread_mutex_unlock(&play_time_lock);	
	};
	return NULL;
}

int main(int argv, char *argc[])
{
	snd_rawmidi_t *MIDI_in = NULL;
	int ret=-1;
	struct timespec taps[BPM_MEASUREPOINTS];
	int tapIndex=0;
	int enoughTaps=0;
	snd_pcm_t *pcm_handle;
	pthread_t playthread_id;


	play_time.tv_sec = 0;

	if (parse_cmdargs(argv, argc)) {
		return -1;
	}

	pcm_handle = init_sound(audio_output, 44100, 2);
	if (NULL == pcm_handle)
		return -1;

	signal(SIGINT,sighandler);

	playthread_id = pthread_create(&playthread_id, NULL, &playsound_thread, pcm_handle);	

	ret = snd_rawmidi_open(&MIDI_in, NULL, midi_input, 0);
	if (ret) {
		printf("ERROR: could not open MIDI input device.\n");
		goto cleanup;
	}

	ret = print_info(MIDI_in); 
	if (ret) {
		printf("ERROR: could not get MIDI device info\n");
		goto cleanup;
	}

	printf("Press CTRL-C to exit\n");
	while(!stop) {

		ret = getMIDImessage(MIDI_in);
		if (ret < 0)
			stop = 1;

		if (ret != NOTE_ON && ret != NOTE_OFF)
			printf("Non-note message recieved. Status: %d\n", ret);

		if (ret == NOTE_ON) {

			clock_gettime(CLOCK_REALTIME, &taps[tapIndex++]);

			pthread_mutex_lock(&play_time_lock);
			clock_gettime(CLOCK_REALTIME, &play_time);
			pthread_mutex_unlock(&play_time_lock);

			if (tapIndex == BPM_MEASUREPOINTS) {
				enoughTaps = 1;
				tapIndex = 0;
			}

			if (enoughTaps && goodtaps < 10) {
				long delta_ms[BPM_MEASUREPOINTS];
				long deltasum_ms=0;
				int i = tapIndex;
				int j = (i + 1) % BPM_MEASUREPOINTS;
				long k = 0;
				long kBPM;				
				int goodTaps = 0;
				do {
					delta_ms[k++] = 1000 * (taps[j].tv_sec - taps[i].tv_sec) +
						(taps[j].tv_nsec - taps[i].tv_nsec) / 1000000;
					
					i = (i + 1) % BPM_MEASUREPOINTS;
					j = (i + 1) % BPM_MEASUREPOINTS;
				} while (j != tapIndex);
			
				for (i = 0; i < k; i ++) {
					deltasum_ms += delta_ms[i];
					printf("%ld ", delta_ms[i]);
				}

				goodTaps = 0;
				deltasum_ms = 0;
				/* Ignore all deltas that is 3% from the median value */
				stupidsort(delta_ms, k);				
				for (i = 0; i < k; i ++) {
					if ( (delta_ms[i] < (delta_ms[k/2]*1030)/1000) &&
					     (delta_ms[i] > (delta_ms[k/2]*970)/1000)) {
						deltasum_ms += delta_ms[i];
						goodTaps++;
					}
				}
				if(goodTaps > 9) {
					beat_period_ms = deltasum_ms / goodTaps; else
				} else {
					beat_period_ms = 0;
				}

				if (beat_period_ms != 0)
					printf("\nbeat interval (ms) %ld \n", beat_period_ms);

				kBPM = goodTaps*60000*1000 / deltasum_ms;
				printf("BPM2: %ld.%ld \n", kBPM / 1000, kBPM % 1000);
				printf("\nGood taps: %d Average delta: %ld \n", goodTaps, deltasum_ms / goodTaps);
			}
		}		
	}
	 	
cleanup:
	snd_pcm_close(pcm_handle);
	if (MIDI_in)
		snd_rawmidi_close(MIDI_in);
	return ret;
}
