# Runs ON THE iPad, NFS root and ./cascadia net on:
#   ssh root@10.55.0.2 sh -s < tools/audio/audio-setup.sh
#
# The userland half of the speakers (the kernel half is the iPadMini card,
# sound/soc/apple/s5l-audio.c + sound/soc/codecs/cs35l19.c):
#   alsa-utils       aplay, speaker-test, amixer, alsamixer
#   /etc/asound.conf "default" mixes every client (dmix) into one 48 kHz
#                    stream, so the desktop and a game can both make sound;
#                    plain hw:0 takes one client at a time
# and a volume of -10 dB on both amps, the card's own default being 0 dB.
# Running it again changes nothing that is already right.
set -e

apk add --no-progress alsa-utils

cat > /etc/asound.conf <<'END'
# The iPad mini 1 speakers (card iPadMini, docs/research/p105-audio.md).
# dmix at 48 kHz, the rate the amps' 12 MHz MCLK divides best; plug in front
# converts whatever a client opens.
pcm.!default {
	type plug
	slave.pcm "speakers"
}
pcm.speakers {
	type dmix
	ipc_key 5105
	ipc_perm 0666
	slave {
		pcm "hw:iPadMini,0"
		rate 48000
		format S16_LE
		channels 2
		period_size 1024
		buffer_size 4096
	}
}
ctl.!default {
	type hw
	card iPadMini
}
END

if [ -e /proc/asound/iPadMini ]; then
	amixer -q -c iPadMini cset name='Left Speaker Volume' 184
	amixer -q -c iPadMini cset name='Right Speaker Volume' 184
fi
echo "audio: ok -- try: speaker-test -c 2 -t sine -l 1"
