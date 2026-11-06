# HoverPreview

Rest your mouse on a photo, video or song in File Explorer (or on the desktop) and a preview pops up next to the cursor, no clicking needed. Move away and it's gone.

- **Photos** are shown scaled to fit, rotated correctly from camera EXIF data, with transparency on a checkerboard. Animated GIFs play.
- **Videos** play right away in the popup and loop, with a progress bar.
- **Music** plays with a small card that shows album art when the file has it.

It's a single 200 KB exe with no installer and nothing extra to install. While idle it uses about 3 MB of private memory and close to 0% CPU.

## Use

Run `build\HoverPreview.exe`. An icon appears in the system tray. Right-click it for options:

| Option | What it does |
| --- | --- |
| Previews on | Pause/resume (or double-click the tray icon) |
| Hover delay | How long the mouse must rest before a preview opens (0.15 / 0.3 / 0.7 s) |
| Preview size | Small, Medium, Large |
| Play sound in videos | Turn off to preview videos muted |
| Start with Windows | Launch automatically at sign-in |

Previews close when the mouse leaves the file, when you click, or when you press Esc. After one preview closes, the next opens almost instantly, so you can glide across a folder.

Explorer's own "Type / Size / Date" tooltip is closed automatically while a preview is open, so it never covers the preview. Everywhere else it works as usual.

## Supported formats

Anything Windows itself can decode:

- Images: jpg, png, gif, bmp, tiff, ico, webp, heic/heif\*, avif\*, jxr, and camera RAW\*
- Video: mp4, m4v, mov, wmv, avi, mkv, webm\*, 3gp, mpg, ts/m2ts
- Audio: mp3, wav, wma, m4a, aac, flac, ogg/opus\*, aiff, amr

\* These need the free codec extension from the Microsoft Store (HEIF, AV1, VP9, Raw Image, Web Media), if it isn't installed already.

## Build

Requires Visual Studio 2019/2022 with the C++ desktop workload.

```
build.bat
```

Output: `build\HoverPreview.exe`.

## How it works

- A background thread checks the cursor position every 50 ms (a few microseconds of work each time). It does real work only once the mouse rests over an Explorer file list or the desktop.
- UI Automation tells it which item is under the cursor. The Explorer tab's folder comes from the Shell's window list, which also covers search results and libraries.
- Images are decoded with WIC. JPEGs are decoded directly at reduced size, so a 24 MP photo takes about 30 ms.
- Video and audio use Media Foundation (the engine Windows uses for its own player). The player is created per preview and destroyed afterwards, and memory is handed back to Windows a few seconds later.

## Troubleshooting

Start it with `HoverPreview.exe --log` to write `%TEMP%\HoverPreview.log`.
`HoverPreview.exe --test "C:\path\file.jpg"` shows a single preview for 5 seconds.
