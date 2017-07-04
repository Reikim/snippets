"neonx" is a project I started back in second year, and have worked on on-and-off since then. The
original idea was simply to copy NXEngine - an open-source reverse-engineering of my favorite
game, Cave Story - in pure C with SDL2 (as opposed to SDL1.2). It was mainly an academic
exercise. Over time it's grown into a more general project to create tools I can chain together
to make 2D games. I've learned a lot from this project about various things - sound, graphics,
physics, and so on.

The samples I've chosen to include here are from the sound submodule, which handles SFX and BGM.
This is the first submodule I started working on, and also the one that's taken the most work; I
had to learn a lot about audio encoding and processing along the way. Mostly, though, it's taken
so long because I've approached it slowly and carefully, taking each decision seriously. (Other
parts weren't written quite the same way, for various real-life reasons...but I digress.)

The `org_mixer` portion is based off of `SDL_Mixer`, a sound-mixing library for SDL, and `SSLib`,
a rewrite of `SDL_Mixer` used in NXEngine. I reviewed both heavily when writing this, essentially
picking and choosing which features I liked from both and building them in. Mostly it was an
educational exercise, but it was also somewhat necessary, since `SSLib` doesn't build correctly
against SDL2, and `SDL2_Mixer` doesn't give an elegant way of putzing about with raw audio streams,
which is necessary since I wanted to use .org files (a custom format used in Cave Story) which
would involve a lot of manual encoding and decoding.

Reviewing this code personally, probably the biggest thing to note is that I'm reasonably happy
with the design decisions that Past Me made, and I'm incredibly happy about the amount of
commenting he did. I've never been a fan of the "self-commenting" idea; there's just too much
additional context that is necessary to really understand a piece of code that *cannot* be encoded
in the code itself. In simpler terms, it may be easy to see *what* a snippet does, but it's way
more important to know *why* it does it there, in that particular way.

Well, it's still a bit excessive. I should probably spin off a separate documentation folder
already...
