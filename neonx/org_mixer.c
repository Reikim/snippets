
#include "org_mixer.h"
#include "../common/retcodes.h"
#include "../common/stack.h"
#include "../common/logging.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <semaphore.h>
#include <errno.h>
#include <limits.h>
#include <string.h>

#include <SDL2/SDL.h>


// TODO: clean up around semaphores. A signal can cause sem_wait to return prematurely, and if
// I'm not careful the current do-while loop can wait forever.

// The mixer is designed following the singleton pattern; i.e. there is only ever one instance of
// the mixer at a time, which is initialized and has methods called from it.
// Hence all of the static declarations.

static SDL_AudioSpec audiospec; // details of audio device linked to this mixer
static int deviceID = 0; // ID of device opened through OpenAudio.
static int initialized = 0; // Make sure we don't double-init

// logging!
static int logHandle = -1;
static int cbHandle = -1;

static mix_channel channels[NUM_CHANNELS]; // array of channels
static sem_t channelLocks[NUM_CHANNELS]; // locks on the mixer channels
// The channels are the sorts of things that we want to avoid access conflicts on.
// Mostly, this is because the mixing callback accesses the channel objects, and the callback can
// execute whenever the bloody hell it pleases.
// It's not disastrous, but...it would be nice to avoid it.
// So, internally, we make sure to lock a channel before fiddling with its settings.
// Externally...the channels are declared static specifically to keep them away from stupidity (mine).
// Need to make sure to do a very minimal amount of work between locks, though. Keeping the callback
// waiting is a Bad Idea.
// TODO: system-independent semaphore thingy? For now, I'll stick to semaphore.h; I'm sure I can
// cross-compile through MinGW if I REALLY need to. (And, in fact, I ended up doing just that...)

static voidstack chunkstacks[NUM_CHANNELS]; // Stacks for chunks...

// Default log filenames. Extern in header.
char * mixerLogname = "mixer.log";
char * callbackLogname = "mixer.callback.log";


/*
	TODO: Implement panning.
 */


// Callback used by the mixer to actually mix the audio.
static void MixCallback (void * UNUSED, uint8_t * stream, int len) {
	logprintf(cbHandle, "Callback called!\n");

	// First order of business: acquire locks on all of the channels
	logprintf(cbHandle, "Acquiring semaphores\n");
	int i, ret;
	for (i = 0; i < NUM_CHANNELS; i++) {
		ret = sem_wait(&channelLocks[i]);
	}
	logprintf(cbHandle, "Acquired\n");

	// Begin by silencing out the stream. In SDL2.0, the stream is not automatically initialized
	// with silence.
	memset((void *)stream, 0, len);

	// Next, begin mixing channels into the stream.
	logprintf(cbHandle, "Beginning mixing\n");
	for (i = 0; i < NUM_CHANNELS; i++) {
		logprintf(cbHandle, "Mixing channel %d\n", i);
		// Need to take len bytes from the channel chunks and mix it into the stream.
		// Or however many bytes we have left, whichever comes first.
		mix_chunk * curChunk = channels[i].chunk;

		if (!channels[i].playing) {
			logprintf(cbHandle, "Channel is not playing, moving to next channel\n");
			continue; // nothin' to do cap'n
		}

		int streampos = 0;
		int bytestogo = len;
		while (bytestogo > 0) { // loop until we've filled the entire buffer
			mix_chunk * curChunk = channels[i].chunk;
			if (curChunk == NULL) {
				logprintf(cbHandle, "No chunk on current channel, moving to next channel\n");
				break; // nothin' to do cap'n; reiterates outer loop
			}

			uint8_t volume = channels[i].volume;//*(curChunk->volume/255);
			char * chunkBuf = curChunk->buf + curChunk->bufpos;
			int buflen = curChunk->buflen - curChunk->bufpos;
			logprintf(cbHandle, "Volume: %d\n", volume);
			logprintf(cbHandle, "streampos: %d\n", streampos);
			logprintf(cbHandle, "bytestogo: %d\n", bytestogo);
			logprintf(cbHandle, "buflen: %d\n", buflen);

			if (buflen > bytestogo) {
				// Mix in buffer, update bufpos, and quit
				logprintf(cbHandle, "buflen > bytestogo, mixing and breaking\n");
				SDL_MixAudioFormat(stream + streampos, chunkBuf, audiospec.format, bytestogo, volume);
				curChunk->bufpos += bytestogo;
				bytestogo = 0;
				break;
			} else if (buflen == bytestogo) {
				logprintf(cbHandle, "buflen == bytestogo, mixing and updating\n");
				// Mix in buffer, update to next chunk, and quit
				SDL_MixAudioFormat(stream + streampos, chunkBuf, audiospec.format, bytestogo, volume);
				bytestogo = 0;
			} else {
				logprintf(cbHandle, "buflen < bytestogo, mixing and updating\n");
				// Mix in what's left, then update to next chunk
				SDL_MixAudioFormat(stream + streampos, chunkBuf, audiospec.format, buflen, volume);
				bytestogo -= buflen;
			}
			// If the code reaches this point, we exhausted the current chunk on this channel.
			// Update to the next chunk. If it's empty, manually break loop.
			// Do whatever else is necessary on chunk-end (see below)
			logprintf(cbHandle, "Exhausted chunk, updating\n");
			streampos += buflen;

			if (curChunk->callback != NULL) {
				logprintf(cbHandle, "Calling chunk callback...");
				mix_chunk * newChunk = (mix_chunk *)curChunk->callback(i, curChunk);
				if (newChunk != NULL) {
					logprintf(cbHandle, "Returned a chunk!\n");
					channels[i].chunk = newChunk;
					continue;
				}
				logprintf(cbHandle, "Returned NULL\n");
			}

			if (curChunk->nextChunk != NULL) {
				logprintf(cbHandle, "Moving to next chunk in the chain\n");
				channels[i].chunk = curChunk->nextChunk;
				channels[i].chunk->bufpos = 0; // Necessary in case a chunk loops back on itself...
				continue;
			} else if (chunkstacks[i].size > 0) {
				logprintf(cbHandle, "Popping old chunk off the stack\n");
				channels[i].chunk = (mix_chunk *)voidpop(&chunkstacks[i]);
			} else {
				logprintf(cbHandle, "No more chunks\n");
				channels[i].chunk = NULL;
			}

			// Free current chunk/buffer if applicable
			if (curChunk->deallocate_buf) {
				free(curChunk->buf);
			}
			if (curChunk->deallocate_me) {
				free(curChunk);
			}
		}
	}
	// Note: Process when chunk is finished on a channel:
	// - Call callback, if not null
	// - If callback returns non-null: play chunk returned
	// - Else if nextChunk not null, begin playing nextChunk
	//   - Deallocate current chunkbuffer/chunk as applicable
	// - Else: check if there are any chunks in interrupt stack. Pop and play if applicable.
	// - Else: null out chunk field; fill rest of buffer with silence.

	// Queueing the same chunk to multiple channels gives undefined behaviour, for now.
	// I can only do so much to prevent shooting myself in the foot...


	// Release all locks and exit the method
	logprintf(cbHandle, "Releasing semaphores...\n");
	for (i = 0; i < NUM_CHANNELS; i++) {
		sem_post(&channelLocks[i]);
	}
	logprintf(cbHandle, "Done mixing\n");
}

mix_chunk * allocate_chunk() {
	mix_chunk * chunk = (mix_chunk *)malloc(sizeof(mix_chunk));

	chunk->buf = NULL;
	chunk->buflen = 0;
	chunk->deallocate_buf = 0;
	chunk->deallocate_me = 0;
	chunk->bufpos = 0;
	chunk->callback = NULL;
	chunk->nextChunk = NULL;

	return chunk;
}

// INITIALIZE
int org_OpenAudio (int frequency, SDL_AudioFormat format, int devicechannels, int chunksize) {

	if (initialized) {
		return rSORRY;
	}

	// Set up logging first. Whee!
	logHandle = log_init(mixerLogname);
	cbHandle = log_init_mode(callbackLogname, _IOFBF);
	logprintf(logHandle, "INITIALIZING MIXER\n");

	/*
		Callback log is IOFBF so that log buffering doesn't unduly affect the callback speed.
		Remember though that the logging module transparently compiles into no-ops for prod
		builds. But I'm a long way away from actually doing that...
	 */

	// Initialize channels and channel locks
	logprintf(logHandle, "Initializing channels...\n");
	int i, ret;
	for (i = 0; i < NUM_CHANNELS; i++) {
		channels[i].chunk = NULL;
		channels[i].volume = 128; // Initialize at max volume
		channels[i].reserved = 0;
		channels[i].playing = 0;
		//channels[i].panning = 0; TODO: panning ain't implemented yet

		ret = sem_init(&channelLocks[i], 0, 1);
		if (ret) {
			logprintf(logHandle, "Unexpected error initializing semaphores! Returning rFAIL...\n");
			return rFAIL;
		}

		chunkstacks[i].size = 0;
		chunkstacks[i].top = NULL;
	}

	// Try to get the correct audio output.
	// As of SDL2, SDL will handle conversion issues before and after handing off to your callback.
	// Unless I find a good reason to deal with those issues myself, I'll let SDL handle it...
	// I should investigate how this actually handles e.g. channels...
	// Still, handy!
	SDL_AudioSpec desired;
	desired.freq = frequency;
	desired.format = format;
	desired.channels = devicechannels;
	desired.samples = chunksize;
	desired.callback = MixCallback;
	desired.userdata = NULL;

	logprintf(logHandle, "Opening audio device...\n");
	deviceID = SDL_OpenAudioDevice(NULL, 0, &desired, &audiospec, 0);
	if (deviceID < 2) { //failure
		logprintf(logHandle, "Error opening audio device!\n");
		logprintf(logHandle, SDL_GetError());
		logprintf(logHandle, "Returning rSDLERR...\n");
		return rSDLERR;
	}
	SDL_PauseAudioDevice(deviceID, 0); // Listen to my song!

	initialized = 1;
	logprintf(logHandle, "Finished initializing mixer.\n");
	return rSUCCESS;
}

/*
	Channel	management functions.

	These all acquire channel locks.
 */

// Find a free channel. Return NUM_CHANNELS if none are open.
int FindFreeChannel(void) {
	int i, ret;
	for (i = 0; i < NUM_CHANNELS; i++) {
		// Acquire lock
		do {
			ret = sem_wait(&channelLocks[i]);
		} while (ret != 0);

		uint8_t reserved = channels[i].reserved;

		sem_post(&channelLocks[i]);

		if (!reserved) {
			return i;
		}
	}
	return NUM_CHANNELS;
}

// Reserve a channel
static int ReserveAnyChannel(void) {
	int i, ret;
	for (i = 0; i < NUM_CHANNELS; i++) {
		do {
			ret = sem_wait(&channelLocks[i]);
		} while (ret != 0);

		if (!channels[i].reserved) {
			channels[i].reserved = 1;
			ret = i;
		}

		sem_post(&channelLocks[i]);

		if (ret) {
			return ret;
		}
	}
	return rSORRY;
}
int ReserveChannel(int channelid) {
	if (channelid >= NUM_CHANNELS) {
		return rBADARG;
	}

	int ret;
	do {
		ret = sem_wait(&channelLocks[channelid]);
	} while (ret != 0);

	if (channels[channelid].reserved) {
		ret = rSORRY;
	} else {
		channels[channelid].reserved = 1;
		ret = channelid;
	}

	sem_post(&channelLocks[channelid]);

	return ret;
}

// Free a channel
void FreeChannel(int channelid) {
	int ret;
	do {
		ret = sem_wait(&channelLocks[channelid]);
	} while (ret != 0);

	channels[channelid].reserved = 1;

	sem_post(&channelLocks[channelid]);
}






/*
	Sound-playing functions
 */

// Play a chunk, overwriting the current one
mix_chunk * PlayChunk(int channelid, mix_chunk * chunk) {
	int ret;
	do {
		ret = sem_wait(&channelLocks[channelid]);
	} while (ret != 0);

	mix_chunk * oldchunk = channels[channelid].chunk;
	chunk->bufpos = 0;
	channels[channelid].chunk = chunk;
	channels[channelid].playing = 1;

	sem_post(&channelLocks[channelid]);

	return oldchunk;
}
// Set up a chunk for playing, overwriting the current one
mix_chunk * SetChunk(int channelid, mix_chunk * chunk) {
	int ret;
	do {
		ret = sem_wait(&channelLocks[channelid]);
	} while (ret != 0);

	mix_chunk * oldchunk = channels[channelid].chunk;
	channels[channelid].chunk = chunk;
	channels[channelid].playing = 1;

	sem_post(&channelLocks[channelid]);

	return oldchunk;
}
// Play a chunk, interrupting the current one (and pushing it onto the stack)
int InterruptChunk(int channelid, mix_chunk * chunk) {
	int ret;
	do {
		ret = sem_wait(&channelLocks[channelid]);
	} while (ret != 0);

	mix_chunk * oldchunk = channels[channelid].chunk;
	channels[channelid].chunk = chunk;
	voidpush(&chunkstacks[channelid], (void *)oldchunk);

	sem_post(&channelLocks[channelid]);

	return rSUCCESS;
}

// Pause channel.
int PauseChannel(int channelid) {
	int ret;
	do {
		ret = sem_wait(&channelLocks[channelid]);
	} while (ret != 0);

	channels[channelid].playing = 0;

	sem_post(&channelLocks[channelid]);

	return rSUCCESS;
}

// Play channel.
int PlayChannel(int channelid) {
	int ret;
	do {
		ret = sem_wait(&channelLocks[channelid]);
	} while (ret != 0);

	channels[channelid].playing = 1;

	sem_post(&channelLocks[channelid]);

	return rSUCCESS;
}

// Set the volume on a channel
uint16_t SetVolume(int channelid, uint8_t volume) {
	// Volume is capped at 128. Thanks SDL.
	if (volume > MAX_VOL) { volume = MAX_VOL; }

	int ret;
	do {
		ret = sem_wait(&channelLocks[channelid]);
	} while (ret != 0);

	uint16_t tmpvol = channels[channelid].volume;
	channels[channelid].volume = volume;

	sem_post(&channelLocks[channelid]);

	return tmpvol;
}

// Set panning for a channel
int8_t SetPanning(int channelid, int8_t panning) {
	return rFAIL; // TODO: not yet implemented
}

// Stop a channel
mix_chunk * StopChannel(int channelid) {
	logprintf(logHandle, "Called StopChannel with ID %d\n", channelid);
	mix_chunk * oldchunk;

	int ret;
	do {
		ret = sem_wait(&channelLocks[channelid]);
	} while (ret != 0);

	oldchunk = channels[channelid].chunk;
	channels[channelid].chunk = NULL;
	channels[channelid].playing = 0;

	sem_post(&channelLocks[channelid]);

	return oldchunk;
}







/*
	Cleanup
 */
void org_CloseAudio() {
	if (!initialized) {
		return; // no use mucking about if we haven't even initialized
	}

	logprintf(logHandle, "CLOSING MIXER\n");

	logprintf(logHandle, "Closing audio device...\n");
	SDL_CloseAudioDevice(deviceID);

	// Release all semaphores
	logprintf(logHandle, "Releasing semaphores...\n");
	int i;
	for (i = 0; i < NUM_CHANNELS; i++) {
		sem_close(&channelLocks[i]);
	}
	deviceID = 0;
	initialized = 0;

	logprintf(logHandle, "Done closing mixer. Have a nice day!\n");
	// Close down logging...
	log_close(logHandle);
	log_close(cbHandle);
}




/*
	Debug funcs. TODO: remove these, or hide them behind a DEBUG macro
 */

int GetDeviceID() {
	return deviceID;
}
int CheckInitialized() {
	return initialized;
}

void GetChannelDetails(int chanNum, mix_channel * dest) {
	int ret;
	do {
		ret = sem_wait(&channelLocks[chanNum]);
	} while (ret != 0);
	void * chanArray = (void *)(channels + chanNum);
	memcpy(chanArray, (void *)dest, sizeof(mix_channel));
	sem_post(&channelLocks[chanNum]);
}
void GetMixerSpec(SDL_AudioSpec * dest) {
	memcpy((void *)dest, (void *)&audiospec, sizeof(SDL_AudioSpec));
}

int GetNumStackedChunks(int channel) {
	return chunkstacks[channel].size;
}
mix_chunk * GetTopChunk(int channel) {
	return (mix_chunk *)chunkstacks[channel].top;
}

