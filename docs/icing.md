# Icing

Icing is a module for VCV Rack 2 that shows [Hydra](https://hydra.ojack.xyz) visuals. Hydra is a live-coded video synthesizer: you describe an image with a few chained functions, and it draws that image many times per second. Icing is an expander for the Filo module. It shows the visuals of the Filo placed directly to its left, as a small preview on its own panel and in a separate output window that can go fullscreen. It also has an AUDIO input, so the visuals can react to sound. You write the Hydra code in Filo's editor, as you would on strudel.cc. This manual covers how Hydra behaves inside Rack. For the Hydra language itself, see [hydra.ojack.xyz](https://hydra.ojack.xyz). For Filo, see [filo.md](filo.md).

## Contents

1. [Overview](#overview)
2. [Quick start](#quick-start)
3. [The panel](#the-panel)
4. [The preview and the output window](#the-preview-and-the-output-window)
5. [Audio reactivity](#audio-reactivity)
6. [Context menu reference](#context-menu-reference)
7. [What is saved with the patch](#what-is-saved-with-the-patch)
8. [Recipes](#recipes)
9. [Limitations](#limitations)
10. [Troubleshooting](#troubleshooting)
11. [Credits and licenses](#credits-and-licenses)

## Overview

Icing does not contain any code editor of its own. The Hydra code lives in the editor of the Filo module that sits directly on Icing's left, next to the Strudel code. When Filo evaluates that code, Hydra starts running, and Icing draws what Hydra produces. Icing is 30 HP wide, and most of its panel is the preview.

Icing finds its Filo by position only. There is no cable between them. If you move Icing away from the Filo, or put another module between them, Icing no longer has a Filo to show and displays the hint "Place directly right of a Filo" instead.

Hydra only starts when your code calls `await initHydra()` or `loadScript()`. The call does not have to be the first line, but the Hydra code that draws must come after it. The [quick start](#quick-start) shows a complete example.

## Quick start

1. Add a Filo module to your patch.
2. Add an Icing module and place it directly to the right of the Filo, with no gap and no other module in between. The hint "Place directly right of a Filo" on Icing's panel disappears once the two are neighbors.
3. Click into Filo's editor and replace the code with this minimal Hydra sketch:

   ```js
   await initHydra()
   osc(10, 0.1, 1).out()
   ```

4. Press the PLAY button on Filo, or press `Cmd/Ctrl+Enter` in the editor. The preview on Icing now shows moving colored stripes.
5. Double-click the preview to open the output window. It opens as an ordinary window that you can move and resize. Use **Fullscreen** in Icing's context menu to fill a whole monitor.

Hydra's visuals are part of the code you evaluate. If you change the code, press PLAY or `Cmd/Ctrl+Enter` again to see the change.

## The panel

The panel shows the name ICING, a preview, and one input.

| Element | Kind | What it does |
|---|---|---|
| ICING | Title | The module name, in the band along the panel. |
| Preview | Display | Shows the image Hydra draws. Double-click it to open the output window. Before a Filo is found, it shows the hint "Place directly right of a Filo". |
| AUDIO | Input | The audio that Hydra analyzes for `a.fft`. Its tooltip reads "Audio for a.fft (unpatched: the Filo's L+R)". |

The AUDIO input takes a normal Rack audio cable. A signal of 5 V counts as the full level that Hydra expects. If you leave AUDIO unpatched, Icing listens to the Filo on its left instead. It takes the average of Filo's left and right outputs. As soon as you plug a cable into AUDIO, that cable is used and Filo's own sound is ignored. Icing has no outputs and no knobs. The preview has no mouse handlers of its own, so you can drag the module and open its context menu with a right-click anywhere on the panel, including the preview.

## The preview and the output window

**The preview.** The preview shows the image of the current Hydra sketch, fitted into its box while keeping the image's proportions. Any space around the image stays black. If your sketch uses several of Hydra's four outputs and calls `render()` without arguments, the preview shows all four outputs side by side in a two-by-two grid. If the sketch calls `render(o1)`, for example, it shows only that output. Before you evaluate any Hydra code, or after the visuals have been stopped, the preview is black.

**The output window.** The output window is a separate window of your operating system that shows the same image, larger. You can open it in two ways. You can double-click the preview, or tick **Output window** in the context menu. If the window is already open but hidden behind other windows, a double-click on the preview brings it to the front. You can move and resize the window like any other. A new window opens at 640 by 360 pixels near the top left of your main monitor. Closing the window with its title bar button only hides it, and the module remembers its position and size.

**Fullscreen.** Tick **Fullscreen** in the context menu to make the output window fill the monitor it is currently on. If the window is open on a second monitor, it fills that second monitor. This is the way to send visuals to a projector. Press `Esc` while the output window has the keyboard focus to leave fullscreen. Fullscreen does not change the video mode of your monitor, so the switch is quick.

**The canvas.** Hydra draws onto a canvas, which is simply the rectangle of pixels it renders. Icing chooses the canvas size so that nothing is stretched.

- While the output window is open and visible, the canvas has the pixel size of that window, up to 4096 pixels on each side. On a high-resolution display, a window can contain more pixels than its size in screen points suggests.
- While the window is closed, hidden or minimized, the canvas has the proportions of the preview, with the long side set to 1280 pixels.
- When you resize the window, Icing waits until the new size has stayed unchanged for about a tenth of a second before it resizes the canvas. The first size is applied at once.
- A larger canvas makes your graphics card work harder, so a fullscreen window on a large monitor is much more demanding than the preview alone.

If your code calls `setResolution(w, h)`, that size is used instead, until you evaluate code without that call. The preview and the window fit the image in either case, keeping its proportions.

## Audio reactivity

Hydra can make visuals follow sound through an object called `a`. Its main value is `a.fft`, a list of numbers that tells you how loud different pitch ranges are right now. FFT stands for Fast Fourier Transform, the calculation that splits a sound into its low, middle and high parts. Each number in the list is one bin, meaning one slice of the pitch range. The first number, `a.fft[0]`, is the lowest slice, and the last number is the highest. Higher numbers mean louder sound in that slice.

The sound comes from the AUDIO input. If AUDIO is unpatched, it comes from the Filo on the left, as the average of its L and R outputs. This means a simple sketch can react to Filo's own drums without any cable. To analyze something else, such as a bassline from another module or a whole mix, patch that signal into AUDIO.

Icing analyzes the most recent 512 samples of the signal and sorts the result into 24 bands that follow how we hear pitch. Hydra's default is four bins, so `a.fft[0]` to `a.fft[3]` exist from the start. The functions `a.setBins()`, `a.setSmooth()`, `a.setCutoff()` and `a.setScale()` all work as described on the Hydra website. See the Hydra website for what each one does. Hydra also provides the shortcut variables `a0`, `a1`, `a2` and `a3` for the first four bins.

```js
await initHydra()
a.setBins(6)
a.setSmooth(0.8)
osc(20, 0.1, () => a.fft[0] * 3)
  .rotate(() => a.fft[3])
  .out()
```

Because the audio is analyzed in Icing, the values only change while something is audible on the chosen signal. With silence, every bin stays at zero.

## Context menu reference

Right-click Icing's panel to open the context menu. Beside the usual Rack entries, Icing adds two items at the bottom. Both are checkboxes.

| Item | Default | What it does |
|---|---|---|
| **Output window** | Off | Opens the output window while ticked, and hides it when unticked. Closing the window with its title bar unticks it again. The state is not saved with the patch, so the window is closed whenever you load a patch. |
| **Fullscreen** | Off | Makes the output window fill its monitor, without a title bar. The menu shows `Esc` next to it, which is the key that leaves fullscreen again. This setting is saved with the patch. |

If **Fullscreen** is ticked while the output window is closed, nothing visible happens until you open the window. It then opens directly in fullscreen.

## What is saved with the patch

Icing saves the geometry of its output window. This consists of the window's position (x and y), its size (width and height) and whether **Fullscreen** is on. When you load the patch, the window reopens at that place once you open it again. If the saved position lies on a monitor that is no longer connected, for example when you load the patch on another computer, the window opens near the top left of your main monitor instead, so that it never ends up off screen.

Icing does not save whether the window was open. It also saves nothing about the visuals. The Hydra code is part of Filo's code, so it is saved with Filo, and Filo evaluates it again when the patch loads.

## Recipes

Each recipe goes into Filo's editor. Press PLAY or `Cmd/Ctrl+Enter` afterward.

**A basic oscillator.** `osc` makes moving stripes. Its three arguments are the number of stripes, the speed, and the amount of color offset. The last line sends the result to output `o0`, the first of Hydra's four outputs.

```js
await initHydra()
osc(15, 0.05, 1.2)
  .kaleid(5)
  .out()
```

**Shapes that follow the audio.** The size of the shape grows with the lowest bin. This works with Filo's own sound, so you can keep a drum pattern running next to it.

```js
await initHydra()
shape(4, () => 0.2 + a.fft[0] * 0.6, 0.01)
  .color(1, 0.4, () => a.fft[2])
  .repeat(3, 3)
  .out()
$: s("bd*4, hh*8")
```

**Using an image.** `s0.initImage()` loads a picture into the source `s0`. Hydra has four such sources, `s0` to `s3`. Images load over https only, and `http://` addresses are upgraded to https. After the first download, the image is kept on disk and also works offline. Until the picture has loaded, the source is transparent black.

```js
await initHydra()
s0.initImage("https://upload.wikimedia.org/wikipedia/commons/thumb/a/a8/Tour_Eiffel_Wikimedia_Commons.jpg/330px-Tour_Eiffel_Wikimedia_Commons.jpg")
src(s0).modulate(osc(5), 0.1).out()
```

**Two outputs side by side.** Hydra has four outputs, `o0` to `o3`. A bare `render()` shows all four in a grid. Here two of them are used, and the empty ones stay black.

```js
await initHydra()
osc(10, 0.1, 0.8).out(o0)
noise(3, 0.1).thresh(0.5).out(o1)
render()
```

To follow Filo's rhythm with the image, use `H(pattern)`, which returns the value of a Strudel pattern at the moment Filo is playing, for example `osc(H("10 20"), 0.1, 1).out()`.

## Limitations

Icing runs Hydra without a web browser, so a few things that need a browser are missing. Where something is not available, the rest of your code still runs and Rack's log records one line about it.

- **Camera, video, screen and stream sources** (`initCam()`, `initVideo()`, `initScreen()`, `initStream()` and `init({src})`) do nothing and leave the source empty, because Icing has no camera or video access.
- **`initImage(url, params)`** ignores `params`, a JPEG's rotation information and colour profiles, because Icing always uses Hydra's default texture settings.
- **`initHydra({feedStrudel})`** leaves `s0` empty, because Filo does not draw Strudel's canvas.
- **The mouse** (`mouse.x` and `mouse.y`) always reads zero, because the preview has no mouse handling.
- **`a.show()`** draws nothing, because Icing has no analyzer overlay.
- **`prev()`** reads the previous frame instead of the buffer currently being drawn.
- **`loadScript()` extensions that need a browser**, such as those using the page, p5.js or three.js, fail. Extensions that only add GLSL functions with `setFunction()` work.
- **The canvas size comes from Icing, not from a browser window.** While the output window is shown, the canvas has the size of the window in pixels, up to 4096 on each side. Otherwise it has the proportions of the preview, with 1280 pixels on the long side. A call to `setResolution()` overrides both, as described [above](#the-preview-and-the-output-window).

## Troubleshooting

**The preview says "Place directly right of a Filo".** Icing has no Filo directly on its left. Move Icing so it touches the right side of a Filo, with no gap and no module between them. A Filo that is further away does not count.

**The preview is black.** Check these in order. First, your code must call `await initHydra()` (or `loadScript()`), and something must draw with `.out()`. Second, evaluate again with PLAY or `Cmd/Ctrl+Enter`. An evaluate without `initHydra()` clears the visuals, and so does Filo's STOP button. The PLAY trigger input on Filo only restarts the transport and does not bring the visuals back. Third, an image source stays transparent until it has loaded, so check your connection and the address. Fourth, a sketch whose frame takes longer than one second is stopped and hushed. Rack's log, `log.txt` in your Rack user folder, names the cause.

**The output window does not open.** Double-click on the preview itself, not on the rest of the panel, or tick **Output window** in the context menu. If it opened and then disappeared, it may be hidden behind another window, so double-click the preview again to bring it forward.

**Visuals are slow or Rack's interface stutters.** Hydra is drawn on your graphics card, and a large canvas costs more. Make the output window smaller, leave fullscreen on a large monitor, or call `setResolution(960, 540)` in your code to use a smaller canvas. Complex chains of `modulate`, `kaleid` and feedback also cost more.

**Fullscreen appears on the wrong monitor.** Fullscreen uses the monitor that holds the middle of the output window. Untick **Fullscreen**, drag the window onto the monitor you want, and tick **Fullscreen** again.

**Esc does not leave fullscreen.** Esc is read from the output window, so click into it first and press `Esc`. You can also untick **Fullscreen** in the menu.

## Credits and licenses

Icing uses [hydra-synth](https://github.com/hydra-synth/hydra-synth), Hydra's own engine, without the browser parts. Hydra is licensed under AGPL-3.0, and its regl, raf-loop and Meyda dependencies are replaced by Icing's own code. The a.fft analysis reproduces Meyda's loudness calculation. Patchisserie itself is licensed under AGPL-3.0-or-later, and the full text is in the `LICENSE` file. The exact versions and license texts of everything bundled are listed in `res/ENGINE_VERSIONS` and `res/ENGINE_LICENSES`. The panel text uses IBM Plex under the SIL Open Font License 1.1.
