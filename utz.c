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
		
	ret = snd_rawmidi_info(MIDI_in, MIDI_info);
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

int main(int argv, char *argc[])
{
	snd_rawmidi_t *MIDI_in = NULL;
	int ret=-1;
	struct timespec taps[BPM_MEASUREPOINTS];
	int tapIndex=0;
	int enoughTaps=0;

	signal(SIGINT,sighandler);	

	/* open default MIDI device, TODO: add command line parameter for this */
	ret = snd_rawmidi_open(&MIDI_in, NULL, "hw:1,0,0", 0); 
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
			printf("UTZ..\n");
	
			clock_gettime(CLOCK_REALTIME, &taps[tapIndex++]);
			if (tapIndex == BPM_MEASUREPOINTS) {
				enoughTaps = 1;
				tapIndex = 0;
			}
				
			if (enoughTaps) {
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

				kBPM = goodTaps*60000*1000 / deltasum_ms;
				printf("BPM2: %ld.%ld \n", kBPM / 1000, kBPM % 1000);
				printf("\nGood taps: %d Average delta: %ld \n", goodTaps, deltasum_ms / goodTaps);
			}
		}		
	}
	 	
cleanup:
	if (MIDI_in)
		snd_rawmidi_close(MIDI_in);
	return ret;
}
