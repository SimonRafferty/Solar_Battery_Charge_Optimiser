This implements an algorithm to calculate how much to charge home batteries over night when the grid power is cheapest.

It's not the best written code - I didn't intend to share it as-is so it's a bit cobbled together.

It uses a strip of addressable LED's to show the current battery SoC and current charge power.
The code will look a bit strange in that it splits the operations into 4 bits, storing the results in EEPROM, then re-booting.
I found that if I ran too many HTTP Post or Get commands one after another, the ESP would crash.  This just splits them up, resets the WiFi and doesn't seem to crash.
The down-side is it makes it hard to follow!

Message me if you have questions & I'll do my best to answer.
