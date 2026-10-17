Models
======

`beat-this-f16.gguf` is part of cyanbox: every beat grid is made with it
(docs/BEATS.md, "With Beat This!"; docs/BEATTHIS.md). It is the network of
Beat This! (Foscarin, Schlüter and Widmer, "Beat this! Accurate beat tracking
without DBN postprocessing", ISMIR 2024; github.com/CPJKU/beat_this), its
`final0` checkpoint in F16 with its batch norms folded, in a GGUF file
(41 MB):

    sha256 cfff4ce4a0c88e01330a8393547f11114d4391ebf522526c8dc08c2538782e49

These weights are the checkpoint's to F16: `beat_this`'s reference code with
them and with the official `final0.ckpt` gives the same beats and downbeats
on a track (the logits within 0.016 of each other, of ±14). cyanbox reads
the file with `src/ml/beatthis.c`; `test_ml_beatthis` checks the logits it
gives against the reference code's with these weights
(`tests/ml/gen_beatthis_ref.py`). `meson install` puts it in
share/cyanbox/models, where cyanbox looks after models/ beside the program;
`CYANBOX_BEATTHIS_MODEL` names another file.

The tagging model (`moss-audio-4b-instruct-q4_k.gguf`, docs/ML.md) is
optional and not in the repository: tagging runs only when asked for.

Beat This!'s code and weights are under the MIT licence:

    MIT License

    Copyright (c) 2024 Institute of Computational Perception, JKU Linz, Austria

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"), to deal
    in the Software without restriction, including without limitation the rights
    to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
    copies of the Software, and to permit persons to whom the Software is
    furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included in all
    copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
    OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
    SOFTWARE.
