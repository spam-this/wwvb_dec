# Introduction

Program wwvb_dec decodes the signal from WWVB receivers such as the
"CANADUINO 60kHz Atomic Clock Receiver Module V3 WWVB MSF JJY60".

It is written for and tested on Raspberry Pi, specicially a Raspberry
Pi Zero W.

Station WWVB broadcasts a time signal on 60 KHz from the USA.  See
https://en.wikipedia.org/wiki/WWVB.  The time data is sent by raising
and lowering a simple carrier signal level once per second.  The
duration of the high power level within a one second internal conveys
either a 1, a 0, or a special "marker" indication.  The markers aid in
framing (finding the start of frames).

The receiver hardware contains an output pin that shows whether the
carrier is in a high or low power state at any particular time.

# Reception of WWVB

The problem is that this simple form of reception is not robust in the
presence of noise.  Unless reception conditions are perfect, there
will be glitches in the signal that complicate decoding.  One cannot
simply look for the expected durations of the 1, 0, and marker
pulses.

For all WWVB clocks, it is recommended for best reception that the
clock be positioned near a window that faces Fort Collins, Colorado
USA such that the clock's antenna is perpendicular to Fort Collins.
Also place the clock far fron sources of RF noise.  Just google "WWVB
reception".  Frankly, many people have problems with WWVB clocks due
to reception issues caused by weak signals and RF noise.

There are also receivers that decode WWVB using the time data it
encodes using binary phase-shift keying.  In theory, this will be more
reliable.  But the author has never tried this type of receiver.

# How wwvc_dec works

wwvb_dec works by capturing 2 minutes of data from the receiver.  This
time guarantees that at least one complete frame (60 seconds, 60
bits/markers, one per second) is present.

The program then searches all possible starting points for the frame
to find the best match.  This is possible because frames contain
several known and unchanging portions: the markers and also several
bits that are "unused" and always sent as zeros.  The best frame is
the one that shows the fewest bit changes (errors) in the a priori
known parts of the frame.

Once the frame is found, the fields of the frame are decoded by
checking if each bit is closest to a 1, a 0, or a marker.

The receiver is sampled 40 times per second.  For example, a perfectly
received "1" looks like this:

   0000000000000000000011111111111111111111

The following is an example of a "1" with 3 errors in samples:

   0000110000000000000011111111111111110111

Using this technique, the program always produced a decode.  But the
values can be wrong.  There is always a "best" match even if the bits
are garbage.

To aid in deciding if the decode is correct or not, the program
produces scores, basically the number of sampled bits that are known to be
incorrect.  If there are relatively few, then the decode is more likely
to be correct.

A reliable radio controlled clock would implement additional
heuristics such as:

*  Do separate decodes a few minutes apart have the correct relationship?
*  Is the decode "sensible" given the clock's internal time estimate?

Program wwvb_dec does none of this.  It is just a little toy, an
exercise in decoding the receiver output.  It is not particularly well
tested or designed for robustness.  Use at your own risk.

Also, wwvb_dec is not written to be super small or efficient.  A
Raspberry Pi handles it easily, but it might be hard for a tiny little
microcontroller.

# Building

It's trivial.  It depends on the Raspberry Pi "pigpio" library so install
development tools and

   sudo apt install libpigpio-dev
   make

The dependence on pigpio is not deep.  All that is needed is

1. A source of microsecond tick (gpioTick())
2. A way to read a GPIO (gpioRead())

# Problems

* Not much test.
* Not robust against tick rollover.
* Probably many more....

Not much effort went into this!
