# Ecosystem Bus (layout v1)

`EcoBus.h` is the one shared module every Ecosystem plug-in includes. Copy this folder
unchanged into each plug-in project; plug-ins built from the same layout version share one
bus inside the DAW process, even though they are separate binaries.

- 24 channels (0 = off). Many senders per channel are summed, many listeners per channel.
- Stereo audio at the session rate; each listener derives what it needs.
- Frames are stored at their timeline sample position (Ecosystem rule §2): a listener reads
  exactly the positions its own output corresponds to, never "the latest block".
- A listener never hears its own sender (no feedback).
- Global channel names, saved by every instance with the project; the most recent edit wins.
- Shared through process memory, located via an environment variable tagged with the
  process id. Nothing is left behind after Reaper quits. Plug-ins in a separate (bridged)
  process get their own bus.

Sender: `claim()` + `setChannel()` + `heartbeat()` on the message thread (about 10-50 Hz),
`write(pos, L, R, n)` on the audio thread. Listener: `read(bus, ch, pos, n, rate, selfId, L, R)`
on the audio thread; it reports how many senders it heard and how many frames were late.

First user: Free Rider v0.2 (ducker listens, Send publishes). Console and FreeSpacer move onto
it next, replacing Console's private strip-number link.
