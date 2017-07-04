
#ifndef ORG_MIX
#define ORG_MIX

#include <stdint.h>
#include <stdio.h>
#include <SDL2/SDL.h>

/*
	Essentially, it's a soundboard, but built entirely in software. More accurately, it's a
	rewrite of SDL_mixer based on SSLib which is itself based on SDL_Mixer. Mostly an academic
	and curiosity project; although it's somewhat necessary, since SSLib won't compile against
	SDL2, and I need a more elegant way of directly manipulating chunks than SDL_Mixer gives me
	since I'll be decoding .org files by hand.
*/

// TODO: find a clever way to allow programmer (me) to decide this at main compile time and
// malloc appropriately. There may be reasons to do that in the future. Until it becomes a
// problem, though, leave it.
#define NUM_CHANNELS 16

// Log file names. Defaults to "mixer.log" and "mixer.callback.log" respectively.
extern char * mixerLogname;
extern char * callbackLogname;

/***********
 * Structs *
 ***********/

/*
	Struct representing a chunk of music to be played.
	At the end of the day a chunk is basically just a buffer full of sound samples.
	TODO: do some struct packing on the bitflags
 */
typedef struct {
	char * buf; // Sample buffer
	int buflen; // Length of buffer
	uint8_t deallocate_buf; // Whether or not to deallocate the buffer after the chunk is finished.
	// There are scenarios where you would want this, believe it or not.
	uint8_t deallocate_me; // Whether or not to deallocate the entire chunk.
	// Believe it or not, you may want this too!
	int bufpos; // Internal position in buffer, in bytes; zero-indexed of course. Don't touch!

	void * (*callback)(int channel, void * chunk); // An optional callback to be called once the chunk is finished
	void * nextChunk;
} mix_chunk;

// The use of bufpos is a takeaway from sslib. afaict it exists because sslib allows you to
// "interrupt" a currently-playing chunk if necessary. bufpos is then necessary so that the position
// doesn't get lost and can be resumed after interrupt. Well, also, it's convenient for actually
// playing a chunk...

// It is assumed that the data in the chunk is appropriate to whatever it's being sent to.
// Thus why we don't store any format data in the chunk.
// Remember, SDL2 handles format differences behind the scenes, so we shouldn't have to do any
// of that work by hand.

// My chunk type differs from SSLib and SDL_mixer in quite a few ways...
// - Instead of tying the callback to the channel, like in SSLib, I allow the callback to be
// tied to the chunk itself. If null, not called.
// - I provide a nextChunk field; in essence, each chunk is actually part of a linked list. When
// the current chunk is finished, after calling the callback, the mixer will look for the next
// chunk in nextChunk. If it's not found, it'll go into idle mode.

// Note that the chunk callback is called within the audio callback; in other words, IT SHOULD
// BE MINIMAL, AND RETURN AS SOON AS POSSIBLE. Ideally, all it should do is chain in the next
// chunk (which was presumably loaded elsewhere); or set some bits to be called later.

// Why'd I make these decisions? Honestly, probably partly to be contrarian. But also because:
// - I wanted to keep SSLib's ability to enqueue multiple chunks
// - I wanted to keep SDL_mixer's option to load an entire song into a single chunk
// - I wanted to keep the mixer fairly separated from the horrors of loading the song
// - I also wanted to keep the interrupt ability of SSLib. More on that later.

// So, some common use cases:
// - I want to play a song, but I want to load it dynamically and not have to get all of it at
// once!
// Load a bit, attach a callback to it, and set it playing. Load the rest and have it ready for
// when the callback is called again; or load only when the callback is called.
// - I want to put in the entire song at once!
// You do you.
// You can also load a single song as a linked list of chunks. Put the callback in the last entry
// so you can be alerted once it's done.

// Essentially, each chunk is part of a queue that the mixer consumes.
// When the mixer is called to interrupt a channel, the current queue gets pushed onto the stack
// and replaced, then popped off after the interrupting chunk is completed.

// Part of the advantage is that this gives flexibility to calling code as to how the chunk queue
// is managed. Is it necessary? Maybe, maybe not. Probably not...but it was amusing to write.


// Struct representing the details of a channel.
// Since I offloaded a bunch of stuff into the chunk definition, this is much smaller than
// the SSLib/SDL_mixer equivalents. For now, anyway.
// This was originally private inside the C source, but I realized it needs to be exposed for
// debugging. I'm kinda dumb sometimes.
typedef struct {
	mix_chunk *chunk; // currently-playing chunk

	uint8_t volume; // Channel volume ranges from 0 (silence) to MAXIMUM NOISE
	// The use of an 8-bit is because of SDL_MixAudio, which takes volumes from 0 to 128.
	uint8_t reserved;
	uint8_t playing; // 0 => paused

	//int8_t panning; // 0 = -128 = centered. 127 = full-right. -127 = full-left.

	// TODO: eventually, stuff for fade-out will be placed here.
	// I don't need that yet, though.
} mix_channel;

#define MAX_VOL 128

#define FULL_RIGHT 127
#define FULL_LEFT -127
#define FULL_CENTER 0


/*********
 * Funcs *
 *********/

mix_chunk * allocate_chunk(); // Allocates a chunk and initializes it to sane defaults.
// Otherwise, setting all of the defaults is a REAL pain.
// Hopefuly, compiler will inline this for me...
// You'll have to free this yourself

/*
	Based on SSInit, but at this point kinda resembles Mix_OpenAudio
	Note: caller is required to call SDL_Init (with audio flags) before this; otherwise, like the
	goggles, it does nothing.

	Params:
	frequency: frequency of output audio in samples/s. This can be adjusted for quality/performance tradeoffs.
	format: see SDL_AudioFormat. This dictates the sample format of chunks.
	channels: Number of audio channels to reserve; e.g. 2 for stereo; again, dictates chunk format
	chunksize: size of audio buffer, in sample frames. See SDL_AudioSpec.samples; shortly, one
		"sample frame" is samplesize*channels bytes. Must be a power of 2, or SDL will kill you.
		Dictates how often the get-more-audio callbacks are called. Set it high for music
		channels, low for SFX channels.

	Returns: rSORRY if the mixer has already been initialized; rSDLERR if there was an issue
		with SDL; rFAIL for weird and unexpected errors; rSUCCESS otherwise.

	TODO: add an overload that allows just throwing in an SDL_AudioSpec manually
 */
int org_OpenAudio(int frequency, SDL_AudioFormat format, int deviceChannels, int chunksize);

// Choosing chunksize is hard. If you set it too high, there'll be a delay before a sound effect
// will play. If you set it too low, audio will skip because the system can't fill the buffer fast
// enough. It's all about choosing a happy medium based on the frequency of your sound and the
// power of your system.
// (In general, though, my intuition is that you want to choose so that you get called at least
// once per frame.)

// Fun fact: SDL2 will handle device/parameter mismatches behind-the-scenes, so you can pretend
// like they don't exist!

/*
	Channel management part the first. Find a free channel, returns integer ID.
	Returns the integer ID, or NUM_CHANNELS if none are available. (Channels are zero-indexed.)
	Do note, though, that even if the channel is free at the time this is called, it's not
	guaranteed to still be free when you touch it again.
	Really, I'm not sure if this method even needs to exist, given we have ReserveAnyChannel...
 */
int FindFreeChannel(void);
/*
	Channel management part the second. Reserve a channel with id. If you pass in anything greater
	than or equal to NUM_CHANNELS, it'll interpret this as "get me any damn channel".
	Returns an error code if the channel has been reserved already. Otherwise, returns the ID of
	the channel reserved.
 */
int ReserveChannel(int channelid);
/*
	Channel management part the last. Free up a channel that's been reserved.
	Regardless of channel state, it's guaranteed to be not reserved at the end of this.
 */
void FreeChannel(int channelid);





/*
	Now here's the interesting stuff. Playing things!
	All of these, of course, acquire channel locks.
 */
/*
	Plays chunk chunk on channel channelid.
	On success, returns the chunk that was overridden. So you can do cleanup if you have to.
	Returns NULL on failure.
	This will OVERWRITE the currently-playing chunkchain. It will not complete, and the chain will
	not be cleaned up even if it's marked for it.
	Implicitly unpauses the channel.
 */
mix_chunk * PlayChunk(int channelid, mix_chunk * chunk);
/*
	As PlayChunk but does NOT unpause the channel.
 */
mix_chunk * SetChunk(int channelid, mix_chunk * chunk);
/*
	As PlayChunk, but INTERRUPTS the currently-playing chunkchain.
	The other chain will resume playing after this one completes.
	Useful if you need n seconds of silence, mostly.
	Returns rSUCCESS on success, error code on failure.
	Does not unpause the channel, since it assumes the channel is currently playing.
 */
int InterruptChunk(int channelid, mix_chunk * chunk);
/*
	Pause channel. The other way of getting n seconds of silence.
 */
int PauseChannel(int channelid);
/*
	Play channel. The inverse of PauseChannel. Separate from PauseChannel because you don't press
	pause to unpause your music player, do you? (At least foobar2k doesn't...)
 */
int PlayChannel(int channelid);
/*
	Set channel volume. Returns the old volume, in case you needed to do something with that.
	(No I don't know what but SSLib does it and it can't hurt. Probably.)
	Channel volume is a uint8_t, with a max volume of 128. (This is because of SDL, don't blame
	me.) Automatically and silently caps volume at 128.
 */
uint16_t SetVolume(int channelid, uint8_t volume);
/*
	Set channel panning. TODO: not implemented
int8_t SetPanning(int channelid, int8_t panning);
 */
/*
	Aborts the specified channel. Basically just wipes the chunk field. Also sets playing = false.
	Returns the chunk that used to be playing.
 */
mix_chunk * StopChannel(int channelid);




// Clean up and go home
void org_CloseAudio();


/*
	Debug funcs. TODO: hide these behind a DEBUG macro, see related task on Trello
 */
int GetDeviceID();
int CheckInitialized();

// This one's an interesting case. I don't want to allow direct access to the channel structs, so
// instead this function will memcpy the channel details into the destination struct. The audiospec
// struct is a similar case.
void GetChannelDetails(int channel, mix_channel * dest);
void GetMixerSpec(SDL_AudioSpec * dest);

// I have less qualms about the chunks themselves, since they're allocated and passed in by the
// surrounding program in the first place. Be careful, and don't fuck up.
int GetNumStackedChunks(int channel);
mix_chunk * GetTopChunk(int channel); // returns a pointer to the top chunk on the stack; NO pop

#endif

