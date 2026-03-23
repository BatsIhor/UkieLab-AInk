# AInk Quick Start Guide

*Your AI agent's window into the physical world.*

---

## What Is AInk?

AInk is a small e-ink display that sits on your desk, shelf, or wall and shows whatever your AI assistant decides you need to see — without you having to ask.

Think of it like a smart whiteboard that your AI keeps updated. Every morning it might show you the weather, your first few meetings, and a note about traffic. Later in the day it could flip to a grocery list, a reminder, or a progress update on something your assistant is working on. You never touch it. You just glance at it.

The screen uses the same technology as an e-reader. It looks like printed paper, works in any lighting, and keeps showing its content even if you unplug it. There is no backlight, no notifications buzzing for your attention, no apps to open. It is simply always there, always current.

---

## What Makes It Different

Most displays are built for humans to operate. AInk is built for AI to operate on your behalf.

Your AI assistant — whether that is Claude, ChatGPT, or any other — can find AInk on your home network automatically, understand what it can show, and update it whenever it has something useful for you. No configuration on your part. No integrations to set up. No dashboard to maintain. You tell your AI what you want to see, and it takes care of the rest.

This is not a smart home hub. It is not another app. It is a quiet, always-visible surface that bridges your AI assistant and your physical space.

---

## In the Box

- AInk display (7.5 inch e-ink screen with ESP32 inside)
- USB-C power cable

That is everything. Plug it in, connect it to WiFi, and it is ready.

---

## Getting Connected

### 1. Plug It In

Use the included USB-C cable and any standard phone charger or USB port. The screen will flicker briefly — that is normal for e-ink — and then show a setup message.

### 2. Connect to the Setup Network

On your phone or laptop, open WiFi settings and look for a network called **UkieLab-AInk**. Connect to it. Your phone may ask if you want to "stay connected" to a network with no internet — say yes.

A setup page will open automatically. If it does not, open a browser and go to `192.168.4.1`.

### 3. Enter Your Home WiFi Details

Type in your home WiFi name and password, then tap **Save**. AInk will disconnect from the setup network, connect to your home WiFi, and show its IP address on screen.

That is it. AInk is now live on your network.

---

## Giving Your AI Access

Once AInk is on your WiFi, tell your AI assistant its IP address — it is displayed on screen right after connecting. Something like:

> "I have an AInk display at 192.168.1.45. Use it to show me useful information."

Your AI will take it from there. It will ask what you'd like to see, figure out how to display it, and update the screen on its own schedule.

If you ever move the display or your router assigns it a new address, just check the screen — the current address is always shown during startup. You can also give it a permanent name (like `aink.local`) so the address never changes, but your AI assistant can handle that setup for you.

---

## Example 1: The Morning Briefing

This is the most popular use. You ask your AI assistant once:

> "Every morning at 7am, update my AInk display with today's weather, my first three calendar events, and what time I need to leave for work."

From that point on, every morning the display refreshes with exactly that — pulled from your calendar, a weather service, and your commute time. You wake up, walk past it while making coffee, and you already know what the day looks like without unlocking your phone.

Here is what a typical morning briefing looks like on screen:

```
┌─────────────────────────────────────────┐
│  THURSDAY, MARCH 12                     │
│                                         │
│  Weather   42F  Rain from 9:30          │
│  Commute   Leave by 8:15               │
│                                         │
│  9:00   Team standup                    │
│  10:30  Design review                   │
│  14:00  1:1 with manager                │
│                                         │
└─────────────────────────────────────────┘
```

No apps. No unlocking. Just a glance.

---

## Example 2: Package & Errand Tracker

Another common setup: keep a running list of things you are waiting for or need to do, updated by your assistant throughout the day.

You might tell your AI:

> "Use my AInk display as a tracker. When a package ships, add it. When it arrives, mark it done. Also add any errands I ask you to remember."

Then as your day unfolds — a shipping notification arrives, you ask your assistant to remember to pick up dry cleaning, a package gets delivered — the display updates automatically:

```
┌─────────────────────────────────────────┐
│  TODAY'S TRACKER                        │
│                                         │
│  [x] Laptop charger — arrived today     │
│  [ ] Books — arriving Thursday          │
│  [ ] Pick up dry cleaning               │
│  [ ] Call dentist re: appointment       │
│                                         │
│  Last updated 2:14 PM                   │
└─────────────────────────────────────────┘
```

The display becomes a shared memory between you and your assistant — a place where things get written down so you do not have to hold them in your head.

---

## Tips

**Place it where you already look.** Next to your coffee machine, on your desk, by the front door. AInk works best as ambient information you absorb without effort. If you have to walk over to check it, it will not stick as a habit.

**Start with one thing.** Ask your assistant for a single daily update first — just the weather, or just your calendar. Add more once you've settled into the habit of glancing at it.

**The screen holds its image without power.** If you unplug it, whatever was last shown stays visible indefinitely. This is normal e-ink behaviour, not a bug.

**It refreshes slowly on purpose.** E-ink takes two to three seconds to update. This is a feature, not a flaw — it means the display is calm and does not flicker like a regular screen. It is meant for information that changes a few times a day, not for live updates every second.

---

## Resetting to Factory Settings

If you move house or change your WiFi, press the reset button three times quickly within ten seconds. The display will return to setup mode and show the **UkieLab-AInk** network again, ready for new WiFi credentials.

---

## Something Not Working?

- **Display shows the setup screen again after connecting** — double-check the WiFi password was typed correctly. Passwords are case-sensitive.
- **AI says it can not find the display** — make sure your phone/computer and AInk are on the same WiFi network. Some routers keep smart home devices on a separate network; check if yours has a "guest network" or "IoT network" setting.
- **Screen looks faint or patchy** — this is normal after the display has been stored for a while. It will clear up after a few refreshes.

---

*Made by **Ihor Bats** at UkieLab — [ukielab.com](https://ukielab.com)*
