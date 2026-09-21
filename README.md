The Modbus RTU sniffer is designed to run on a ESP32.

In addition you need a hardware adpter that transform RS485 to 3.3V Serial. There is no write funtionality implemented.
The software uses the standard wireless connection via wlan access or local accesspoint.

The information flow is:
modbus -> RS485 adapter -> ESP <-> wireless network <-> browser

There is no Security implemented to access the Sniffer Server. Also the Web Socket connection and data exchange is open to the connected network. When the ESP is operated as an Access Point, there is a limitation that only one client can connect to the ESP.

The software was developed under arduino with some help of Google-AI. So neither Google nor I will take responibility or any relaiabilty for the code or functionality.

The sniffer provides (please take a look at the Modbus Sniffer.pdf)
- A rough statistic overview with register types and used addresses.
- A monitor to filter out the transfered data for 4 selected function codes and addresses.
- A sniffer for 100 commuication packets. The sniffer could be started manually oder via a defined bus pattern.
- The data shown in the browser can also be downloaded as a JSON file

The software uses some basic task management from the RTOS.
- Task 1 capture data from the serial bus. Checks for frame end and frame length.
- Task 2 get these data and can do some real time fast basic prcesses, i.e CRC checks or hardware outputs.
- the standard main loop is doing the major data inspection, handling and forwarding to the browser.
- in addtion there is an short ISR to capture the timestamp. Therefor it is necessary to connet PIN 4 and 17 (RX).
- on the USB Port there is some output:
  - in access point mode: SSID and PASSWORD and ...
  - Modbus data

If Pin 22 is low after reset or no SSID is configured the ESP runs into the access point mode.

The function codes 3,4 and 6 are tested with real hardware. All other FCs are implemented with a limted testing.
 
