# mars-sdl

SDL2 port of a legacy DOS Mars landscape renderer.

This project is a line-by-line C translation of the original assembly renderer in `MARS.ASM`, as was disassembled by Wojciech Bruzda in his work [MARS.EXE → COM](https://chaos.if.uj.edu.pl/~wojtek/MARS.COM/).
My motivation was my memories of the old DOS MARS rendered, originally written by Tim J. Clarke, which at the time pushed my CPU quite a lot.

This implementation is based on SDL2 for window creation, input, and presenting the software-rendered indexed-color framebuffer.

The renderer keeps the original functionality of the renderer. AI tools have been used for the translation, but additional manual work was required to correct various introduced bugs which turned the process a non-trivial one.

## Build

### Prerequisites

SDL2 library

Normal build and run:

```sh
make
./mars_sdl_port
```

## Goal

This project is the result of an experiment. The purpose of this is not to redesign the renderer, but to preserve the behavior of the original program while making the translated code easy to build, inspect, and run on today's systems.
