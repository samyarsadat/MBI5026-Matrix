> [!WARNING]
> This write-up is W.I.P. and unfinished. There may be typos, grammatical errors or factual errors. It has not been properly proof-read. Diagrams will be added as well.

<br>

# MBI5026 16x40 LED Display
In November 2023, I got my hands on a number of 16x40 LED matrix displays produced by a company called "Enforma". There was no documentation provided with these displays, but they had RJ-11 connectors on their backs labeled `RS-485`. This gave me hope, at first, as it implied that no driver circuitry was needed and that I only needed to send data to the displays over a serial connection.

On closer inspection, I found 6 ICs on the matrix PCBs. They were all labeled with `MBI5026CD`. After a quick Google search, I was able to find their datasheet. According to the datasheet, the `MBI5026` was a "16-bit constant-current LED sink driver". This tells us a lot about what these chips do, and it also indicated to me that there must be some kind of external control circuitry somewhere, but we will get to that later.

<br>

## The MBI5026
An LED sink driver (or current-sink driver) is one of two main LED driver types (the other being source/current-source driver). Electrical current is "sunk" to these drivers, as the name might suggest, and so their outputs are connected to the cathodes of the LEDs. The `MBI5026`, specifically, is also a constant-current LED driver, which means that it keeps the current flowing through the LEDs constant, regardless of the LED's bias voltage (the current is set using an external resistor attached to a dedicated pin on the IC).

Another important feature of these ICs is that they handle inputs serially, similar to a shift register. This makes it rather simple for them to be daisy-chained (as they are in this matrix), and it allows for a clever optimization when it comes to sending data to them using microcontrollers. The optimization in question is using the built-in SPI hardware to send data instead of bit-banging. This allows for data to be sent at much higher rates and, of course, frees up many CPU cycles, something we need a lot of.

The LED matrix modules used are configured such that the cathodes of the LEDs are all attached to each other in columns. This means that our `MBI5026` ICs control the columns of the module. So the cathodes of all 16 LEDs in each column are connected to one output on one of the three (per color – one set for red, one set for green) `MBI5026`s. 

Okay, now we can control the columns, but what about the rows? Well... that’s where things get a little bit more complicated. I mentioned above that I initially thought no extra control circuitry was needed for the display but that I was wrong and I’d explain later. Later is now, so I shall explain. 

After finding out what the `MBI5026`s were, I tried to look for some type of current-source driver for the anodes of the LEDs, but I found none. Instead, I managed to trace the anodes of the displays to a pair of connectors on the back of the PCB. I also then traced two of the pins (data pins, as the other two were connected to GND and VCC) of the aforementioned RJ-11 to another connector on the back, and the data, clock, and latch pins of the `MBI5026`s to a fourth connector.

These discoveries made it clear to me that there must have been some kind of control board attached to this display PCB using those four connectors, and that that board contained the rest of the circuitry required for making the display work.

<br>

## Driving The Displays
### Hardware
With all of this infomation in hand, I devised a simple circuit to drive these displays using the Arduino UNO R3 (ATMega328P). The circuit consists of 16 P-channel MOSFETs for driving the anodes of the matrices. As mentioned before, the anodes of the displays are linked together in rows, and these displays have 16 rows, hence we need 16 MOSFETs.

Now, after we consider the SPI connections for the `MBI5026`s, we aren't left with the 16 pins we need to drive the MOSFETs, but we also don't really need 16 pins. It would be a waste to connect all MOSFETs directly anyway as we don't need to be able to activate them two-at-a-time, instead, we can use a 16-channel multiplexer.

I chose the `74HC4067`, as it's one I was already familiar with, and it had decent specifications for this application. The `74HC4067` is a 16-channel analog multiplexer, but any 16-channel mux with good enough switching characteristics will do just fine, it doesn't even have to be an analog mux, as we're only controlling MOSFETs.

Most P-channel MOSFETs will also work just fine, you could even use PNP transistors, and you'd probably be fine. As we're multiplexing the rows, each row is only held on for around 850 microseconds (you can change this by changing `display_row_on_delay_us` in the code), so there isn't any continuous current draw. The most amount of continuous current draw I observed when holding all pixels in one row one was no more than 400mA.

Note that there are 10k ohm pull-up resistors connected to the gate pins of every MOSFET to ensure that their outputs are turned off when left floating (which is the case, unless they're selected by the mux).

As mentioned before, we'll be using the Arduino's SPI hardware for sending data to the `MBI5026`s. The six `MBI5026`s are divided into twp groups of three, one group for red, and the other for green (the pixels are dual-color), note that the anodes for the green and red LEDs are again connected to eachother, so we still only need 16 MOSFETs for driving both colors. This works because their cathodes are connected to different `MBI5026`s, of course.

As far as the cathodes are concerned, we can treat the display as two separate displays, one being the green, and the other red. But there's just one problem with this approach... the Arduino UNO R3 only has one set of SPI pins, not two. The solution to this problem is quite trivial. The `MBI5026`s, in addition to their SDI (Serial Data In) and CLK (Clock) pins, also have an LE (labeled as "Data Strobe Input Terminal" in the datasheet, but I presume it stands for Latch Enable) pin. 

You see, data from the input shift register isn't "committed" to the output drivers of the `MBI5026` unless a pulse is sent on the LE pin. This is very handy, as we can connect the SDI and CLK pins of both `MBI5026` groups together, but still maintain the ability to separately control both colors.

Basically, what we can do is the following:
 - Send the data for the green group over SPI,
 - Pulse the LE pin of the green group, "committing" the desired green pixel states to the output drivers,
 - Send the data for the red group over the same SPI bus,
 - Pulse the LE pin of the red group, "committing" the desired red pixel states to the output drivers.

These steps are repeated for each row, of course, as we are multiplexing the display one-row-at-a-time. More about the software implementation below.

<br>

### Software
The software uses PlatformIO and is written in C++. I opted to use Adafruit's GFX library for text and graphics "rendering" (i.e. generating pixel data from text and/or shape drawing commands, etc.), as I saw no need to implement that myself when there's a perfectly usable and readily available implementation already.

Instead, I focused on optimizing the lower level details of driving the display (taking data from a set of pixel buffers, one for red and one for green, and displaying them properly). It was a rather enjoyable experience, as I got to chase down millisecond-level improvements in execution time, which is a level of optimization I often don't or can't pursue. 

I will, by no means, claim to have written the _most_ optimized driver for these displays, but I certainly made an attempt, even performing direct register manipulations to avoid function call overhead at some points!

In the example I've provided, all of the text "rendering" takes place during the setup phase of the program. This introduces a challenge when one tries to implement scrolling text. Re-rendering the pixel buffer every scroll cycle is very expensive (CPU cycles-wise, and hence, time-wise) and so results in a lot of flickering.

You could simply use a more powerful microcontroller to solve this issue, such as the RP2040/RP2350 from Raspberry Pi, or even a microcontroller from the STM32 series (such as the STM32F103, for example), but that would be cheating! Not that this is a competition, but still, what fun would that have been?

So I devised a rather simple solution, one which I'm sure is used in some real LED display drivers as well... we can make our display buffers longer (width-wise) than the size of our actual displays, render the entirety of the contents we want to scroll on the display once at startup (and write it to the buffers, of course), and then simply shift our view of the buffer to the right by one pixel every n-milliseconds when actually displaying the contents on the screen.

Also, the scrolling offset counter is incremented by a hardware timer interrupt. This will be important later.

Now, there is _some_ extra overhead compared to just displaying a static view, but it's _significantly_ less than shifting the cursor position and re-computing the pixel data on every scroll cycle. The maximum width for our "extended buffer" is only 256 pixels, however, so whatever it is that we want to scroll must fit wihtin those 256 pixels.

This limitation arises from the maximum limit of an unsigned 8-bit integer (255), as you may have noticed. Theoretically, we can expand the buffer beyond this by changing some variable types to 16-bit unsigned integers, and I did try this, however this results in many operations taking more CPU cycles (as the ATMega328P is an 8-bit processor with 8-bit wide registers and an 8-bit ALU, and so operations that would take a single instruction with 8-bit variables take more with 16-bit ones), which results in more flickering.

To explain the why longer execution times result in flickering, I must first explain how the driver code works in greater detail. Here are the tasks performed by the program when drawing a single frame, in the order in which they are completed:
 - Interrupts are disabled to prevent the scroll offset from being incremented during a frame draw, which would result in tearing.
 - For each of the 16 rows, the following operations are repeated:
   - Set the mux output (IO) pin to logic high, turning off the row that's currently on (the previous row, unless the current row is 0, in which case it would be the last row, 15).
   - **[GREEN GROUP]** Read the 5 bytes (5 * 8 = 40 bits, which is the width of the display in pixels) required for display from the buffer, taking into account the scroll offset if scrolling is enabled.
   - Send those 5 bytes to the `MBI5026`s over SPI.
   - **[GREEN GROUP]** Pulse the latch pin, committing the data we just sent to the output buffers of the `MBI5026`s for this group.
   - **[RED GROUP]** Read the 5 bytes required for display from the buffer, taking into account the scroll offset if scrolling is enabled.
   - Send those 5 bytes to the `MBI5026`s over SPI.
   - **[RED GROUP]** Pulse the latch pin, committing the data we just sent to the output buffers of the `MBI5026`s for this group.
   - Set the mux output address to the address of our current row (the address determines which one of the 16 channels is connected through to the mux's IO pin).
   - Set the mux output (IO) pin to logic low, turning on the current row.
   - Wait for 850 microseconds. If we don't wait here, the program will continue to the next iteration of the loop immediately, turning off the row we just turned on, which would make the display very dim for obvious reasons.
 - Enable interrupts again to allow for the scroll counter to be incremented using the timer ISR.

To be continued...