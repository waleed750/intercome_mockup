# RNNoise, vendored

`xiph/rnnoise` at tag `v0.1.1`, unmodified, BSD licensed (see `COPYING`).

This is the last release that carries its model in source (`rnn_data.c`);
later ones fetch the weights from xiph.org at build time, which a panel
being provisioned on a customer's network cannot rely on. The model is the
one trained by Mozilla in 2017 on speech at poor signal-to-noise ratios,
which is exactly what this panel's microphone delivers: speech about 9 dB
above its own floor. A band-by-band suppressor cannot take that floor
without taking the quiet consonants with it; this one can.

It runs at 48 kHz on 10 ms frames. `src/audio/denoise.c` wraps it with the
speexdsp resampler either side, so the rest of the engine keeps its 8 kHz
frames.
