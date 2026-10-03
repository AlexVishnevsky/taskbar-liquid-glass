# Taskbar Liquid Glass

A [Windhawk](https://windhawk.net/) mod that puts a real lens behind the
Windows 11 taskbar. The screen content under the taskbar is captured, blurred
and bent along the edge of the bar, so windows and wallpaper refract through
it instead of only being blurred.

Мод для Windhawk: справжнє заломлююче скло під панеллю завдань Windows 11.
Те, що лежить під панеллю, захоплюється, розмивається і вигинається по краю,
а не просто розмивається. Інструкція українською — [нижче](#українською).

> There is no screenshot here on purpose: the glass and the taskbar are
> excluded from screen capture (see [Limitations](#limitations)), so a
> screenshot cannot show them.

## What is in this repository

| File | What it is |
| --- | --- |
| `taskbar-liquid-glass.wh.cpp` | The mod itself. |
| `mod-settings.json` | The mod settings used with the theme below. |
| `taskbar-styler-theme.json` | A custom **Windows 11 Taskbar Styler** theme made for this glass: full-width bar, transparent background, gradient edge stroke, rounded flyouts. |

## How it works

Taskbar themes can only blur and tint: XAML brushes cannot move pixels. So the
glass is drawn in a separate click-through window placed right below the
taskbar in the Z order:

1. The monitor is captured with Windows.Graphics.Capture. The glass window is
   excluded from the capture so it does not see itself.
2. The strip under the taskbar is copied out of each frame and blurred with
   Direct2D. Frames that changed nothing under the taskbar are skipped.
3. A displacement map is generated for the shape of the bar: neutral in the
   middle, growing towards the edge along the edge normal.
4. The blurred strip goes through `D2D1DisplacementMap`, is clipped to the
   rounded rectangle, tinted, and presented with DirectComposition.

Each edge picks its direction by itself. An edge with screen content next to
it pulls that content into the rim. An edge that is flush with the monitor
border has nothing beyond it, so it bends the content from inside the glass.

The capture → blur → displacement pipeline follows the Liquid Glass material
of [Island Media Controls](https://github.com/usho-lear/island-media-controls)
by usho (MIT).

## Install

Requires Windows 11 and Windhawk. Tested on Windows 11 25H2 (build 26200) with
Windhawk 1.7.3.

1. In Windhawk choose **Create a new mod**, replace the template with the
   contents of `taskbar-liquid-glass.wh.cpp`, then **Compile Mod** and
   **Exit Editing Mode**.
2. Make the taskbar background transparent, otherwise the glass stays hidden
   behind it. Either:
   - import the whole theme: open **Windows 11 Taskbar Styler** → **Advanced**
     → **Mod settings**, paste the contents of `taskbar-styler-theme.json` and
     save; or
   - keep your own theme and add one control style for the target
     `Taskbar.TaskbarFrame > Grid#RootGrid > Taskbar.TaskbarBackground > Grid > Rectangle#BackgroundFill`
     with the style `Fill=Transparent`.
3. Optional: open the mod → **Advanced** → **Mod settings** and paste
   `mod-settings.json`.

Changing Taskbar Styler settings can restart Explorer once; it comes back on
its own.

## Settings

| Setting | Default | Meaning |
| --- | --- | --- |
| `refraction` | 45 | How far the image is shifted at the very edge, in px. Negative swaps the directions. 0 turns refraction off. |
| `bezelWidth` | 18 | Depth of the lens band from the edge, in px. |
| `blur` | 4 | Blur strength, in px. |
| `cornerRadius` | 6 | Corner radius of the glass, in px. |
| `tintColor` | `#1A090909` | Tint over the glass, `#AARRGGBB`. |
| `rimHighlight` | 0 | Extra light rim, in %. Leave 0 if the taskbar theme already draws a stroke. |
| `marginLeft` … `marginBottom` | 0 | Insets of the glass from the taskbar window, in px. Use them for dock-style themes. |
| `excludeTaskbarFromCapture` | on | Keeps the taskbar's own icons out of the glass. See below. |
| `debugDump` | off | Saves one rendered glass frame to `%TEMP%\taskbar-liquid-glass.bmp`. |

Pixel values are scaled with the display DPI.

## Limitations

- The glass window is always hidden from screenshots, screen recording and
  screen sharing.
- With `excludeTaskbarFromCapture` on, the taskbar is hidden from them too.
  Turning it off brings the taskbar back into screenshots, but its icons then
  show up blurred inside the glass.
- Only the primary taskbar is handled.
- The glass is a rounded rectangle. It does not follow shapes that other mods
  draw inside the taskbar.
- The capture runs for as long as the taskbar is visible.

## Українською

1. У Windhawk: **Створити новий мод**, замінити шаблон вмістом
   `taskbar-liquid-glass.wh.cpp`, скомпілювати й вийти з редактора.
2. Зробити фон панелі прозорим, інакше скла не видно. Або імпортувати всю
   тему: **Windows 11 Taskbar Styler** → **Додатково** → **Налаштування
   мода**, вставити вміст `taskbar-styler-theme.json`. Або у власній темі
   задати `Fill=Transparent` для
   `Taskbar.TaskbarFrame > Grid#RootGrid > Taskbar.TaskbarBackground > Grid > Rectangle#BackgroundFill`.
3. За бажанням вставити `mod-settings.json` у налаштування мода.

Скло й панель завдань не потрапляють на скриншоти та в демонстрацію екрана.
Панель можна повернути, вимкнувши «Ховати панель завдань від захоплення», але
тоді її значки проступатимуть розмитими у склі.

## License

MIT, see [LICENSE](LICENSE).
