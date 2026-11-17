# LAME (libmp3lame 3.100)

The MP3 encoder behind Export > MP3. This is the unmodified `libmp3lame` source from
https://lame.sourceforge.io (lame-3.100.tar.gz), minus the decoder (`mpglib`), the assembly
and the command-line front end. Only `config.h` and this file are ours.

LAME is licensed under the LGPL (see `COPYING` and `LICENSE`). MicFX is AGPLv3, which is
compatible; the source is right here, so you can rebuild or replace it. Built as the static
library `micfx_lame` in the top-level CMakeLists.txt.

MP3 encoding may need a patent licence in some countries; the MP3 patents have expired in most.
