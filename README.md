# KillfeedOverlay
Simple overlay for PS2, that displays the last one who killed you (last bullet HS or not). And top 3 who killed you the most.
Overlay written in .cpp with no additional libs, meaning that the .exe can be run without anything extra.

## Features
* Shows name of the last who killed you.
* Shows whether last bullet was a HS or not (HS/DEADS)
* Tracks how many times you've died to this person this session.
* Displays top 3 who have killed you the most with their (HS/DEADS).
* Lots of config options.

To change stuff, simply change the config.json
```
{
  "service_id": "s:sealobster",		// api key to use toward the census.daybreakgames.com
  "character_name": "Sealobster", // Name of player to track

  "window": { "x": 400, "y": 10, "w": 360, "h": 260, "alpha": 255 }, // Window position and size
  "transparent_bg": true, // To use transparent background
  "chroma_r": 255, 		  // Transparent filter, used to filter out the background
  "chroma_g": 0,		  // Transparent filter, used to filter out the background
  "chroma_b": 255,		  // Transparent filter, used to filter out the background
  
  "max_rows": 8,		  // Max rows in the window
  "min_deaths_to_list": 1,// Minimum required deaths by player to display it
  "window_size": 5,		  // Window size for the text
  "flash_seconds": 6,	  // Used when flushing the after each grab
  "lock_position": true,  // To make the window locked into its position, cant click and drag
  "always_on_top": true,  // Make the window always on top
  "skip_environment": false, // Used when stream returns null, setting this to true makes it dont check for any errors
  "poll_ms": 1000  // How often it sud fetch data from the api
}
```
