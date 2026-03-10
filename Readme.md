So, here is what happened. I have been replaying Celeste recently (which is a great game and highly recommended). I discovered that the developers had also created a free, experimental 3D spinoff called Celeste 64, and I wanted to try it! However, I am weird in that I use a very old version of macOS, specifically OS X 10.9 Mavericks. The spinoff needed macOS 12, almost a decade newer.

I was about to go to sleep for the night anyway, so, on a lark, I downloaded the game, opened Claude Code with `--dangerously-skip-permissions` (I can always restore my system via Time Machine), and told it:

> Do whatever it takes to get this game to run on this OS.

After watching Claude churn for a bit, I stopped it and said that it would need to write some polyfills, and told it about the MacPorts Legacy Support library. Then I went to sleep.

I woke up the next morning to a working game. I was really surprised!

The game did initially crash when a controller was connected, and when saving. I asked Claude to fix these things, and it did.

This seemed interesting enough that I wanted to share it. I also asked Claude to "create a writeup of everything you did", which is saved as COMPAT_WRITEUP.md.

---

Celeste 64 binaries are distributed under the terms of the original license: https://github.com/EXOK/Celeste64

All other code is licensed under the WTFPL.